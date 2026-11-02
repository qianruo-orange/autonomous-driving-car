/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "control/image_tracking.h"
#include "yolo/yolo_features.h"
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define HISTORY 32
#define LOST_MS 800
#define FRESH_MS 1000
#define REACQUIRE_MS 5000
#define ID_SLOTS (YOLO_MAX_DETS * 2)
/* Initial detector-neck cosine gates, distinct from the removed histogram
 * thresholds. These require video/identity calibration; this is not trained ReID. */
#define FEATURE_NEAR .70
/* Conservative far gate from the current small regression set, not a
 * general ReID accuracy guarantee: different same-class neck ROIs can be .99. */
#define FEATURE_FAR .995
#define FEATURE_ADAPT .95
#define FEATURE_NEAR_MARGIN .04
#define FEATURE_FAR_MARGIN .06
#define MOTION_MARGIN .08
#define FEATURE_ALPHA .02

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    yolo_result_t frame;
    unsigned long long capture_ms;
    unsigned long long object_ids[YOLO_MAX_DETS];
} history[HISTORY];
static unsigned cursor;
static image_tracking_snapshot_t current;
static yolo_det_t tracked;
static unsigned long long last_seen;
static double velocity_x, velocity_y;
static yolo_feature_t reference, adaptive;
typedef struct {
    yolo_det_t box;
    unsigned long long stamp;
    int hits;
} pending_match_t;
static pending_match_t pending;
static struct {
    unsigned long long id, stamp;
    yolo_det_t box;
    double vx, vy;
    yolo_feature_t reference, adaptive;
} identities[ID_SLOTS];
static unsigned long long next_object_id;

typedef struct {
    double distance, iou, ratio, gate;
    int nearby;
} motion_t;

static unsigned long long monotonic_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int box_valid(const yolo_det_t *b, int w, int h)
{
    return b->cls >= 0 && b->cls < YOLO_MAX_CLASSES && isfinite(b->conf) &&
           b->conf >= 0 && b->conf <= 1 && isfinite(b->x1) && isfinite(b->y1) &&
           isfinite(b->x2) && isfinite(b->y2) && b->x1 >= 0 && b->y1 >= 0 &&
           b->x2 <= w && b->y2 <= h && b->x2 > b->x1 && b->y2 > b->y1;
}
static double center_x(const yolo_det_t *b) { return (b->x1 + b->x2) * .5; }
static double center_y(const yolo_det_t *b) { return (b->y1 + b->y2) * .5; }
static double area(const yolo_det_t *b) { return (b->x2 - b->x1) * (b->y2 - b->y1); }

static motion_t motion(const yolo_det_t *before, const yolo_det_t *after,
                       double vx, double vy, double dt, int w, int h)
{
    double dx = vx * dt, dy = vy * dt;
    double distance = hypot((center_x(after) - center_x(before) - dx) / w,
                            (center_y(after) - center_y(before) - dy) / h);
    double iw = fmax(0, fmin(after->x2, before->x2 + dx) - fmax(after->x1, before->x1 + dx));
    double ih = fmax(0, fmin(after->y2, before->y2 + dy) - fmax(after->y1, before->y1 + dy));
    double intersection = iw * ih, ratio = area(after) / area(before);
    double gate = fmin(.25, .06 + .5 * dt);
    return (motion_t){distance, intersection / (area(before) + area(after) - intersection),
                       ratio, gate, dt <= LOST_MS / 1000.0 && distance <= gate && ratio >= .25 && ratio <= 4};
}
static double score(const motion_t *m, double similarity, int has_feature)
{
    double distance = fmin(1, m->distance / m->gate);
    double size = fmin(1, fabs(log(m->ratio)) / log(4));
    return has_feature ? .75 * (1 - similarity) + .15 * distance + .08 * (1 - m->iou) + .02 * size :
                         .65 * (1 - m->iou) + .25 * distance + .10 * size;
}
static void velocity_update(const yolo_det_t *before, const yolo_det_t *after,
                             double dt, double *vx, double *vy)
{
    if (dt > 0) {
        *vx = .5 * *vx + .5 * (center_x(after) - center_x(before)) / dt;
        *vy = .5 * *vy + .5 * (center_y(after) - center_y(before)) / dt;
    }
}

static void reset_locked(void)
{
    memset(history, 0, sizeof(history));
    memset(identities, 0, sizeof(identities));
    memset(&reference, 0, sizeof(reference));
    memset(&adaptive, 0, sizeof(adaptive));
    memset(&pending, 0, sizeof(pending));
    memset(&tracked, 0, sizeof(tracked));
    memset(&current, 0, sizeof(current));
    current.selected = -1;
    current.appearance_similarity = -1;
    cursor = 0;
    last_seen = 0;
    velocity_x = velocity_y = 0;
}
static void identity_store(int slot, int detection, unsigned long long id)
{
    const yolo_det_t *d = &current.frame.dets[detection];
    if (identities[slot].id == id && identities[slot].stamp < current.capture_ms) {
        double dt = (current.capture_ms - identities[slot].stamp) / 1000.0;
        motion_t m = motion(&identities[slot].box, d, identities[slot].vx, identities[slot].vy,
                            dt, current.frame.w, current.frame.h);
        if (m.nearby) velocity_update(&identities[slot].box, d, dt, &identities[slot].vx, &identities[slot].vy);
        else identities[slot].vx = identities[slot].vy = 0;
    } else {
        identities[slot].vx = identities[slot].vy = 0;
        identities[slot].reference = identities[slot].adaptive = d->feature;
    }
    if (!identities[slot].reference.valid && d->feature.valid)
        identities[slot].reference = identities[slot].adaptive = d->feature;
    else if (yolo_feature_similarity(&identities[slot].reference, &d->feature) >= FEATURE_ADAPT)
        yolo_feature_blend(&identities[slot].adaptive, &d->feature, FEATURE_ALPHA);
    identities[slot].id = id;
    identities[slot].stamp = current.capture_ms;
    identities[slot].box = *d;
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
/* All visible detections receive one-to-one IDs from short-range, velocity-
 * predicted associations. The explicitly selected ID is reserved during loss. */
static void assign_object_ids(void)
{
    const yolo_result_t *f = &current.frame;
    unsigned char used[ID_SLOTS] = {0}, blocked[YOLO_MAX_DETS] = {0};
    unsigned char has_feature[YOLO_MAX_DETS][ID_SLOTS] = {{0}};
    double costs[YOLO_MAX_DETS][ID_SLOTS];
    memset(current.object_ids, 0, sizeof(current.object_ids));
    if (current.active && current.selected >= 0 && current.track_id) {
        int slot = -1;
        for (int j = 0; j < ID_SLOTS; ++j)
            if (identities[j].id == current.track_id) { slot = j; break; }
        if (slot < 0) slot = identity_free_slot(used);
        if (slot >= 0) { identity_store(slot, current.selected, current.track_id); used[slot] = 1; }
    }
    for (int i = 0; i < f->count; ++i) for (int j = 0; j < ID_SLOTS; ++j) {
        costs[i][j] = 1e9;
        const yolo_det_t *a = &f->dets[i];
        if (current.object_ids[i] || used[j] || !identities[j].id ||
            (current.active && identities[j].id == current.track_id) ||
            current.capture_ms < identities[j].stamp || current.capture_ms - identities[j].stamp > LOST_MS ||
            !box_valid(a, f->w, f->h) || a->cls != identities[j].box.cls) continue;
        double dt = (current.capture_ms - identities[j].stamp) / 1000.0;
        motion_t m = motion(&identities[j].box, a, identities[j].vx, identities[j].vy, dt, f->w, f->h);
        if (!m.nearby) continue;
        int feature = identities[j].reference.valid && a->feature.valid;
        double original = yolo_feature_similarity(&identities[j].reference, &a->feature);
        double recent = yolo_feature_similarity(&identities[j].adaptive, &a->feature);
        double similarity = .7 * original + .3 * recent;
        if (feature && (original < FEATURE_NEAR || similarity < FEATURE_NEAR)) continue;
        has_feature[i][j] = feature;
        costs[i][j] = score(&m, similarity, feature);
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
        if (alternative - best < (has_feature[row][column] ? FEATURE_NEAR_MARGIN : MOTION_MARGIN)) {
            blocked[row] = 1; used[column] = 1; continue;
        }
        unsigned long long id = identities[column].id;
        identity_store(column, row, id);
        used[column] = 1;
    }
    for (int i = 0; i < f->count; ++i) {
        if (current.object_ids[i] || !box_valid(&f->dets[i], f->w, f->h)) continue;
        int slot = identity_free_slot(used);
        if (slot >= 0) { identity_store(slot, i, ++next_object_id); used[slot] = 1; }
    }
}

/* Descriptor similarity is the primary score. Distant reacquisition requires
 * a stronger immutable-reference match and two spatially consistent frames. */
static int associate(void)
{
    const yolo_result_t *f = &current.frame;
    current.appearance_similarity = -1;
    if (!current.active || current.capture_ms < last_seen ||
        current.capture_ms - last_seen > (reference.valid ? REACQUIRE_MS : LOST_MS)) {
        pending.hits = 0; return -1;
    }
    double dt = (current.capture_ms - last_seen) / 1000.0;
    double best = 1e9, second = 1e9;
    int selected = -1, needs_confirmation = 0, selected_feature = 0;
    double selected_similarity = -1, selected_original = -1;
    for (int i = 0; i < f->count; ++i) {
        const yolo_det_t *b = &f->dets[i];
        if (!box_valid(b, f->w, f->h) || b->cls != tracked.cls) continue;
        motion_t m = motion(&tracked, b, velocity_x, velocity_y, dt, f->w, f->h);
        int feature = reference.valid && b->feature.valid;
        double original = yolo_feature_similarity(&reference, &b->feature);
        double recent = yolo_feature_similarity(&adaptive, &b->feature);
        double similarity = .7 * original + .3 * recent;
        if (feature) {
            double gate = m.nearby ? FEATURE_NEAR : FEATURE_FAR;
            if (original < gate || similarity < gate || m.ratio < .20 || m.ratio > 5) continue;
        } else if (!m.nearby) continue;
        double cost = score(&m, similarity, feature);
        if (cost < best) {
            second = best; best = cost; selected = i;
            needs_confirmation = !m.nearby; selected_feature = feature;
            selected_similarity = similarity; selected_original = original;
        } else if (cost < second) second = cost;
    }
    double margin = selected_feature ? (needs_confirmation ? FEATURE_FAR_MARGIN : FEATURE_NEAR_MARGIN) : MOTION_MARGIN;
    if (selected < 0 || second - best < margin) { pending.hits = 0; return -1; }
    const yolo_det_t *b = &f->dets[selected];
    if (needs_confirmation) {
        double distance = hypot((center_x(b) - center_x(&pending.box)) / f->w,
                                (center_y(b) - center_y(&pending.box)) / f->h);
        if (!pending.hits || current.capture_ms <= pending.stamp || current.capture_ms - pending.stamp > 300 ||
            distance > .08 || b->cls != pending.box.cls ||
            yolo_feature_similarity(&pending.box.feature, &b->feature) < FEATURE_NEAR) pending.hits = 0;
        pending.box = *b; pending.stamp = current.capture_ms;
        if (++pending.hits < 2) return -1;
        ++current.reidentifications; velocity_x = velocity_y = 0;
    } else velocity_update(&tracked, b, dt, &velocity_x, &velocity_y);
    pending.hits = 0;
    tracked = *b; last_seen = current.capture_ms;
    if (!reference.valid && b->feature.valid) reference = adaptive = b->feature;
    else if (!needs_confirmation && selected_original >= FEATURE_ADAPT && second - best >= FEATURE_FAR_MARGIN)
        yolo_feature_blend(&adaptive, &b->feature, FEATURE_ALPHA);
    current.reference_ready = reference.valid;
    current.appearance_similarity = selected_feature ? selected_similarity : -1;
    return selected;
}

int image_tracking_update(const yolo_result_t *frame, const yolo_classes_t *classes,
                          unsigned long long capture_ms, unsigned long long *object_ids)
{
    if (object_ids) memset(object_ids, 0, sizeof(unsigned long long) * YOLO_MAX_DETS);
    if (!frame || frame->count < 0 || frame->count > YOLO_MAX_DETS || frame->w <= 0 || frame->h <= 0 ||
        !frame->seq || !capture_ms) return -1;
    yolo_result_t clean = *frame;
    int available = 0;
    for (int i = 0; i < frame->count; ++i) {
        memset(&clean.dets[i].feature, 0, sizeof(clean.dets[i].feature));
        if (box_valid(&frame->dets[i], frame->w, frame->h))
            yolo_feature_blend(&clean.dets[i].feature, &frame->dets[i].feature, 1);
        available += clean.dets[i].feature.valid;
    }
    pthread_mutex_lock(&lock);
    if (current.capture_ms && capture_ms <= current.capture_ms) { pthread_mutex_unlock(&lock); return -1; }
    if (frame->seq <= current.frame.seq || frame->w != current.frame.w || frame->h != current.frame.h)
        reset_locked();
    current.frame = clean;
    current.capture_ms = capture_ms;
    current.features_available = available;
    for (int i = 0; i < YOLO_MAX_DETS; ++i) current.feature_ready[i] = i < frame->count && clean.dets[i].feature.valid;
    if (classes) current.classes = *classes;
    current.selected = associate();
    assign_object_ids();
    history[cursor % HISTORY].frame = clean;
    memcpy(history[cursor % HISTORY].object_ids, current.object_ids, sizeof(current.object_ids));
    history[cursor++ % HISTORY].capture_ms = capture_ms;
    if (object_ids) memcpy(object_ids, current.object_ids, sizeof(current.object_ids));
    int selected = current.selected;
    pthread_mutex_unlock(&lock);
    return selected;
}
void image_tracking_snapshot(image_tracking_snapshot_t *out)
{
    if (!out) return;
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
        if (!stamp || f->seq != seq || now < stamp || now - stamp > LOST_MS || index < 0 || index >= f->count ||
            !history[i].object_ids[index] || !box_valid(&f->dets[index], f->w, f->h)) continue;
        image_tracking_snapshot_t before = current;
        yolo_det_t old_box = tracked;
        unsigned long long old_seen = last_seen;
        double old_vx = velocity_x, old_vy = velocity_y;
        yolo_feature_t old_reference = reference, old_adaptive = adaptive;
        pending_match_t old_pending = pending;
        tracked = f->dets[index]; last_seen = stamp;
        velocity_x = velocity_y = 0;
        current.active = 1; reference = adaptive = f->dets[index].feature;
        current.reference_ready = reference.valid;
        current.reidentifications = pending.hits = 0;
        current.appearance_similarity = reference.valid ? 1 : -1;
        current.selected = current.frame.seq == seq ? index : associate();
        if (current.selected < 0 && !reference.valid) {
            current = before; tracked = old_box; last_seen = old_seen;
            velocity_x = old_vx; velocity_y = old_vy;
            reference = old_reference; adaptive = old_adaptive; pending = old_pending;
        } else { current.track_id = history[i].object_ids[index]; rc = 0; }
        break;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}
void image_tracking_stop(void)
{
    pthread_mutex_lock(&lock);
    current.active = 0; current.selected = -1;
    current.reference_ready = reference.valid = adaptive.valid = pending.hits = 0;
    current.reidentifications = 0; current.appearance_similarity = -1;
    pthread_mutex_unlock(&lock);
}
void image_tracking_reset(void)
{
    pthread_mutex_lock(&lock);
    reset_locked();
    pthread_mutex_unlock(&lock);
}
