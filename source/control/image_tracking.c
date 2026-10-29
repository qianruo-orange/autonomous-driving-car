/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "control/image_tracking.h"
#include "control/appearance_features.h"
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define HISTORY 32
#define LOST_MS 800
#define FRESH_MS 1000
#define REACQUIRE_MS 5000
#define ID_SLOTS (YOLO_MAX_DETS * 2)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    yolo_result_t frame;
    unsigned long long capture_ms;
    appearance_feature_t features[YOLO_MAX_DETS];
    int has_rgb;
    unsigned long long object_ids[YOLO_MAX_DETS];
} history[HISTORY];
static unsigned cursor;
static image_tracking_snapshot_t current;
static yolo_det_t tracked;
static unsigned long long last_seen;
static double velocity_x, velocity_y;
static appearance_feature_t reference, adaptive, features[YOLO_MAX_DETS];
typedef struct {
    yolo_det_t box;
    unsigned long long stamp;
    int hits;
} pending_match_t;
static pending_match_t pending;
static struct {
    unsigned long long id, stamp;
    yolo_det_t box;
    appearance_feature_t feature;
} identities[ID_SLOTS];
static unsigned long long next_object_id;

static unsigned long long monotonic_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int box_valid(const yolo_det_t *b, int w, int h)
{
    return b->cls >= 0 && b->cls < YOLO_MAX_CLASSES && isfinite(b->x1) &&
           isfinite(b->y1) && isfinite(b->x2) && isfinite(b->y2) &&
           b->x1 >= 0 && b->y1 >= 0 && b->x2 <= w && b->y2 <= h &&
           b->x2 > b->x1 && b->y2 > b->y1;
}
static double center_x(const yolo_det_t *b) { return (b->x1 + b->x2) * .5; }
static double center_y(const yolo_det_t *b) { return (b->y1 + b->y2) * .5; }
static double area(const yolo_det_t *b) { return (b->x2 - b->x1) * (b->y2 - b->y1); }

static void identity_store(int slot, int detection, unsigned long long id)
{
    identities[slot].id = id;
    identities[slot].stamp = current.capture_ms;
    identities[slot].box = current.frame.dets[detection];
    identities[slot].feature = features[detection];
    current.object_ids[detection] = id;
}
static int identity_free_slot(const unsigned char *used)
{
    int slot = -1;
    for (int j = 0; j < ID_SLOTS; ++j) {
        if (used[j] || (current.active && identities[j].id == current.track_id)) continue;
        if (slot < 0 || identities[j].stamp < identities[slot].stamp) slot = j;
    }
    return slot;
}
/* Give every visible detection one ID using a one-to-one match. The explicitly
 * selected appearance track owns its ID, including during temporary loss. */
static void assign_object_ids(void)
{
    const yolo_result_t *f = &current.frame;
    unsigned char used[ID_SLOTS] = {0}, blocked[YOLO_MAX_DETS] = {0};
    double costs[YOLO_MAX_DETS][ID_SLOTS];
    memset(current.object_ids, 0, sizeof(current.object_ids));
    if (current.active && current.selected >= 0 && current.track_id) {
        int slot = -1;
        for (int j = 0; j < ID_SLOTS; ++j)
            if (identities[j].id == current.track_id) { slot = j; break; }
        if (slot < 0) slot = identity_free_slot(used);
        if (slot >= 0) {
            identity_store(slot, current.selected, current.track_id);
            used[slot] = 1;
        }
    }
    for (int i = 0; i < f->count; ++i) for (int j = 0; j < ID_SLOTS; ++j) {
        costs[i][j] = 1e9;
        const yolo_det_t *a = &f->dets[i], *b = &identities[j].box;
        if (current.object_ids[i] || used[j] || !identities[j].id ||
            (current.active && identities[j].id == current.track_id) ||
            current.capture_ms < identities[j].stamp ||
            current.capture_ms - identities[j].stamp > LOST_MS ||
            !box_valid(a, f->w, f->h) || a->cls != b->cls) continue;
        double dt = (current.capture_ms - identities[j].stamp) / 1000.0;
        double distance = hypot((center_x(a) - center_x(b)) / f->w,
                                (center_y(a) - center_y(b)) / f->h);
        double ratio = area(a) / area(b);
        if (distance > fmin(.25, .08 + .5 * dt) || ratio < .25 || ratio > 4) continue;
        double similarity = appearance_similarity(&identities[j].feature, &features[i]);
        if (identities[j].feature.valid && similarity < .82) continue;
        double iw = fmax(0, fmin(a->x2, b->x2) - fmax(a->x1, b->x1));
        double ih = fmax(0, fmin(a->y2, b->y2) - fmax(a->y1, b->y1));
        double intersection = iw * ih;
        double iou = intersection / (area(a) + area(b) - intersection);
        costs[i][j] = .7 * (1 - iou) + 2 * distance + (similarity >= 0 ? .5 * (1 - similarity) : 0);
    }
    for (int pass = 0; pass < f->count; ++pass) {
        double best = 1e9;
        int row = -1, column = -1;
        for (int i = 0; i < f->count; ++i) if (!current.object_ids[i] && !blocked[i])
            for (int j = 0; j < ID_SLOTS; ++j) if (!used[j] && costs[i][j] < best) {
                best = costs[i][j]; row = i; column = j;
            }
        if (row < 0) break;
        double alternative = 1e9;
        for (int j = 0; j < ID_SLOTS; ++j)
            if (!used[j] && j != column && costs[row][j] < alternative) alternative = costs[row][j];
        for (int i = 0; i < f->count; ++i)
            if (!current.object_ids[i] && !blocked[i] && i != row && costs[i][column] < alternative)
                alternative = costs[i][column];
        if (alternative - best < .025) { blocked[row] = 1; used[column] = 1; continue; }
        unsigned long long id = identities[column].id;
        identity_store(column, row, id);
        used[column] = 1;
    }
    for (int i = 0; i < f->count; ++i) {
        if (current.object_ids[i] || !box_valid(&f->dets[i], f->w, f->h)) continue;
        int slot = identity_free_slot(used);
        if (slot >= 0) {
            identity_store(slot, i, ++next_object_id);
            used[slot] = 1;
        }
    }
}

/* Motion and appearance jointly associate detections. An immutable selection
 * reference vetoes lookalikes and gates slow template adaptation. Reacquisition
 * outside the motion gate needs stronger appearance and two confirming frames. */
static int associate(void)
{
    const yolo_result_t *f = &current.frame;
    current.appearance_similarity = -1;
    if (!current.active || current.capture_ms < last_seen ||
        current.capture_ms - last_seen > (reference.valid ? REACQUIRE_MS : LOST_MS)) {
        pending.hits = 0;
        return -1;
    }
    double dt = (current.capture_ms - last_seen) / 1000.0;
    double px = center_x(&tracked) + velocity_x * dt;
    double py = center_y(&tracked) + velocity_y * dt;
    double gate = fmin(.22, .06 + .5 * dt);
    double best = 1e9, second = 1e9;
    int selected = -1, needs_confirmation = 0;
    double selected_similarity = -1;
    for (int i = 0; i < f->count; ++i) {
        const yolo_det_t *b = &f->dets[i];
        if (!box_valid(b, f->w, f->h) || b->cls != tracked.cls)
            continue;
        double dx = (center_x(b) - px) / f->w, dy = (center_y(b) - py) / f->h;
        double distance = hypot(dx, dy), ratio = area(b) / area(&tracked);
        int nearby = dt <= LOST_MS / 1000.0 && distance <= gate && ratio >= .25 && ratio <= 4;
        double original = appearance_similarity(&reference, &features[i]);
        double recent = appearance_similarity(&adaptive, &features[i]);
        double similarity = .7 * original + .3 * recent;
        if (reference.valid) {
            if (original < .78 || recent < .80 || similarity < .82 || ratio < .20 || ratio > 5)
                continue;
            if (!nearby && (original < .94 || similarity < .94)) continue;
        } else if (!nearby) continue;
        double iw = fmax(0, fmin(b->x2, tracked.x2) - fmax(b->x1, tracked.x1));
        double ih = fmax(0, fmin(b->y2, tracked.y2) - fmax(b->y1, tracked.y1));
        double intersection = iw * ih;
        double iou = intersection / (area(b) + area(&tracked) - intersection);
        double score = reference.valid ? (1 - similarity) +
                         (nearby ? .1 * distance + .03 * (1 - iou) : .05) :
                         .65 * (1 - iou) + 2 * distance + .1 * fabs(log(ratio));
        if (score < best) {
            second = best;
            best = score;
            selected = i;
            needs_confirmation = !nearby;
            selected_similarity = similarity;
        } else if (score < second) second = score;
    }
    if (selected < 0 || second - best < (reference.valid ? (needs_confirmation ? .05 : .025) : .08)) {
        pending.hits = 0;
        return -1;
    }
    if (needs_confirmation) {
        const yolo_det_t *b = &f->dets[selected];
        double distance = hypot((center_x(b) - center_x(&pending.box)) / f->w,
                                (center_y(b) - center_y(&pending.box)) / f->h);
        if (!pending.hits || current.capture_ms <= pending.stamp ||
            current.capture_ms - pending.stamp > 300 || distance > .08 || b->cls != pending.box.cls)
            pending.hits = 0;
        pending.box = *b;
        pending.stamp = current.capture_ms;
        if (++pending.hits < 2) return -1;
        ++current.reidentifications;
    }
    pending.hits = 0;
    if (selected >= 0) {
        const yolo_det_t *b = &f->dets[selected];
        if (needs_confirmation) velocity_x = velocity_y = 0;
        else if (dt > 0) {
            velocity_x = .5 * velocity_x + .5 * (center_x(b) - center_x(&tracked)) / dt;
            velocity_y = .5 * velocity_y + .5 * (center_y(b) - center_y(&tracked)) / dt;
        }
        tracked = *b;
        last_seen = current.capture_ms;
        if (reference.valid) {
            current.appearance_similarity = selected_similarity;
            if (!needs_confirmation && appearance_similarity(&reference, &features[selected]) >= .92 &&
                second - best >= .05)
                appearance_blend(&adaptive, &features[selected], .02);
        }
    }
    return selected;
}

int image_tracking_update(const yolo_result_t *frame, const yolo_classes_t *classes,
                          unsigned long long capture_ms, const unsigned char *rgb, size_t rgb_bytes,
                          unsigned long long *object_ids)
{
    if (object_ids) memset(object_ids, 0, sizeof(unsigned long long) * YOLO_MAX_DETS);
    if (!frame || frame->count < 0 || frame->count > YOLO_MAX_DETS ||
        frame->w <= 0 || frame->h <= 0 || !capture_ms)
        return -1;
    appearance_feature_t extracted[YOLO_MAX_DETS] = {0};
    int available = 0;
    for (int i = 0; rgb && i < frame->count; ++i)
        if (!appearance_extract(rgb, rgb_bytes, frame->w, frame->h, &frame->dets[i], &extracted[i]))
            ++available;
    pthread_mutex_lock(&lock);
    if (current.capture_ms && capture_ms <= current.capture_ms) {
        pthread_mutex_unlock(&lock);
        return -1;
    }
    if (frame->seq <= current.frame.seq || frame->w != current.frame.w ||
        frame->h != current.frame.h) {
        memset(history, 0, sizeof(history));
        memset(identities, 0, sizeof(identities));
        current.active = 0;
        reference.valid = adaptive.valid = pending.hits = 0;
        current.reference_ready = current.reidentifications = 0;
    }
    current.frame = *frame;
    current.capture_ms = capture_ms;
    current.features_available = available;
    for (int i = 0; i < YOLO_MAX_DETS; ++i) current.feature_ready[i] = extracted[i].valid;
    memcpy(features, extracted, sizeof(features));
    if (classes) current.classes = *classes;
    current.selected = associate();
    assign_object_ids();
    history[cursor % HISTORY].frame = *frame;
    memcpy(history[cursor % HISTORY].features, features, sizeof(features));
    history[cursor % HISTORY].has_rgb = rgb != NULL;
    memcpy(history[cursor % HISTORY].object_ids, current.object_ids, sizeof(current.object_ids));
    history[cursor++ % HISTORY].capture_ms = capture_ms;
    if (object_ids) memcpy(object_ids, current.object_ids, sizeof(current.object_ids));
    int selected = current.selected;
    pthread_mutex_unlock(&lock);
    return selected;
}
void image_tracking_snapshot(image_tracking_snapshot_t *out)
{
    pthread_mutex_lock(&lock);
    *out = current;
    unsigned long long seen = last_seen;
    pthread_mutex_unlock(&lock);
    unsigned long long now = monotonic_ms();
    out->age_ms = now >= out->capture_ms ? now - out->capture_ms : FRESH_MS + 1;
    out->fresh = out->capture_ms && out->age_ms <= FRESH_MS;
    if (!out->fresh) out->selected = -1;
    if (out->selected < 0) out->appearance_similarity = -1;
    out->reacquire_remaining_ms = out->active && out->reference_ready && now >= seen &&
        now - seen < REACQUIRE_MS ? REACQUIRE_MS - (now - seen) : 0;
}
int image_tracking_select(unsigned long long seq, int index)
{
    int rc = -1;
    unsigned long long now = monotonic_ms();
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < HISTORY; ++i) {
        const yolo_result_t *f = &history[i].frame;
        unsigned long long stamp = history[i].capture_ms;
        if (!stamp || f->seq != seq || now < stamp || now - stamp > LOST_MS ||
            index < 0 || index >= f->count || !box_valid(&f->dets[index], f->w, f->h))
            continue;
        if (history[i].has_rgb && !history[i].features[index].valid) break;
        image_tracking_snapshot_t before = current;
        yolo_det_t old_box = tracked;
        unsigned long long old_seen = last_seen;
        double old_vx = velocity_x, old_vy = velocity_y;
        appearance_feature_t old_reference = reference, old_adaptive = adaptive;
        pending_match_t old_pending = pending;
        tracked = f->dets[index];
        last_seen = stamp;
        velocity_x = velocity_y = 0;
        current.active = 1;
        reference = adaptive = history[i].features[index];
        current.reference_ready = reference.valid;
        current.reidentifications = pending.hits = 0;
        current.appearance_similarity = reference.valid ? 1 : -1;
        current.selected = current.frame.seq == seq ? index : associate();
        // A valid appearance enrollment remains useful if the clicked object
        // has moved or become occluded by the time this request arrives. Let
        // subsequent frames confirm its identity within the recovery window.
        if (current.selected < 0 && !reference.valid) {
            current = before;
            tracked = old_box;
            last_seen = old_seen;
            velocity_x = old_vx;
            velocity_y = old_vy;
            reference = old_reference;
            adaptive = old_adaptive;
            pending = old_pending;
        } else {
            current.track_id = history[i].object_ids[index];
            rc = 0;
        }
        break;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}
void image_tracking_stop(void)
{
    pthread_mutex_lock(&lock);
    current.active = 0;
    current.selected = -1;
    current.reference_ready = reference.valid = adaptive.valid = pending.hits = 0;
    current.reidentifications = 0;
    current.appearance_similarity = -1;
    pthread_mutex_unlock(&lock);
}
