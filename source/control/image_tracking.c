/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "control/image_tracking.h"
#include <math.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define HISTORY 32
#define LOST_MS 800
#define FRESH_MS 1000
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    yolo_result_t frame;
    unsigned long long capture_ms;
} history[HISTORY];
static unsigned cursor;
static image_tracking_snapshot_t current;
static yolo_det_t tracked;
static unsigned long long last_seen;
static double velocity_x, velocity_y;

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

/* Same-class association using overlap, predicted center and size. Ambiguous
 * matches pause instead of jumping to another object; no appearance re-ID. */
static int associate(void)
{
    const yolo_result_t *f = &current.frame;
    if (!current.active || current.capture_ms < last_seen ||
        current.capture_ms - last_seen > LOST_MS)
        return -1;
    double dt = (current.capture_ms - last_seen) / 1000.0;
    double px = center_x(&tracked) + velocity_x * dt;
    double py = center_y(&tracked) + velocity_y * dt;
    double gate = fmin(.22, .06 + .5 * dt);
    double best = 1e9, second = 1e9;
    int selected = -1;
    for (int i = 0; i < f->count; ++i) {
        const yolo_det_t *b = &f->dets[i];
        if (!box_valid(b, f->w, f->h) || b->cls != tracked.cls)
            continue;
        double dx = (center_x(b) - px) / f->w, dy = (center_y(b) - py) / f->h;
        double distance = hypot(dx, dy), ratio = area(b) / area(&tracked);
        if (distance > gate || ratio < .25 || ratio > 4)
            continue;
        double iw = fmax(0, fmin(b->x2, tracked.x2) - fmax(b->x1, tracked.x1));
        double ih = fmax(0, fmin(b->y2, tracked.y2) - fmax(b->y1, tracked.y1));
        double intersection = iw * ih;
        double iou = intersection / (area(b) + area(&tracked) - intersection);
        double score = .65 * (1 - iou) + 2 * distance + .1 * fabs(log(ratio));
        if (score < best) {
            second = best;
            best = score;
            selected = i;
        } else if (score < second) second = score;
    }
    if (second - best < .08)
        return -1;
    if (selected >= 0) {
        const yolo_det_t *b = &f->dets[selected];
        if (dt > 0) {
            velocity_x = .5 * velocity_x + .5 * (center_x(b) - center_x(&tracked)) / dt;
            velocity_y = .5 * velocity_y + .5 * (center_y(b) - center_y(&tracked)) / dt;
        }
        tracked = *b;
        last_seen = current.capture_ms;
    }
    return selected;
}

int image_tracking_update(const yolo_result_t *frame, const yolo_classes_t *classes,
                          unsigned long long capture_ms)
{
    if (!frame || frame->count < 0 || frame->count > YOLO_MAX_DETS ||
        frame->w <= 0 || frame->h <= 0 || !capture_ms)
        return -1;
    pthread_mutex_lock(&lock);
    if (current.capture_ms && capture_ms <= current.capture_ms) {
        pthread_mutex_unlock(&lock);
        return -1;
    }
    if (frame->seq <= current.frame.seq || frame->w != current.frame.w ||
        frame->h != current.frame.h) {
        memset(history, 0, sizeof(history));
        current.active = 0;
    }
    current.frame = *frame;
    current.capture_ms = capture_ms;
    if (classes) current.classes = *classes;
    history[cursor % HISTORY].frame = *frame;
    history[cursor++ % HISTORY].capture_ms = capture_ms;
    current.selected = associate();
    int selected = current.selected;
    pthread_mutex_unlock(&lock);
    return selected;
}
void image_tracking_snapshot(image_tracking_snapshot_t *out)
{
    pthread_mutex_lock(&lock);
    *out = current;
    pthread_mutex_unlock(&lock);
    unsigned long long now = monotonic_ms();
    out->age_ms = now >= out->capture_ms ? now - out->capture_ms : FRESH_MS + 1;
    out->fresh = out->capture_ms && out->age_ms <= FRESH_MS;
    if (!out->fresh) out->selected = -1;
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
        image_tracking_snapshot_t before = current;
        yolo_det_t old_box = tracked;
        unsigned long long old_seen = last_seen;
        double old_vx = velocity_x, old_vy = velocity_y;
        tracked = f->dets[index];
        last_seen = stamp;
        velocity_x = velocity_y = 0;
        current.active = 1;
        current.selected = current.frame.seq == seq ? index : associate();
        if (current.selected < 0) {
            current = before;
            tracked = old_box;
            last_seen = old_seen;
            velocity_x = old_vx;
            velocity_y = old_vy;
        } else {
            ++current.track_id;
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
    pthread_mutex_unlock(&lock);
}
