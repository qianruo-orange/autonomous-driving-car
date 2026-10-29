/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cc -std=c11 -Iinclude tests/image_tracking_test.c source/control/image_tracking.c source/control/appearance_features.c
 *    -pthread -lm -o /tmp/image_tracking_test && /tmp/image_tracking_test */
#define _POSIX_C_SOURCE 200809L
#include "control/image_tracking.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

static unsigned long long test_ms = 10000;
int clock_gettime(clockid_t clock, struct timespec *out)
{
    assert(clock == CLOCK_MONOTONIC);
    out->tv_sec = test_ms / 1000;
    out->tv_nsec = (test_ms % 1000) * 1000000;
    return 0;
}
static yolo_det_t box(int cls, float x)
{
    return (yolo_det_t){.cls = cls, .conf = .9f, .x1 = x, .y1 = 100,
                         .x2 = x + 100, .y2 = 300};
}
static int update(yolo_result_t *f, unsigned elapsed)
{
    test_ms += elapsed;
    ++f->seq;
    return image_tracking_update(f, NULL, test_ms, NULL, 0, NULL);
}
int main(void)
{
    yolo_result_t f = {.w = 1000, .h = 600, .count = 2};
    f.dets[0] = box(0, 100);
    f.dets[1] = box(0, 600);
    assert(update(&f, 20) == -1);
    image_tracking_snapshot_t s;
    image_tracking_snapshot(&s);
    assert(s.object_ids[0] == 1 && s.object_ids[1] == 2);
    assert(image_tracking_select(f.seq, 0) == 0);
    assert(image_tracking_select(f.seq, 32) == -1);
    /* Detection ordering changes while the same object moves. */
    f.dets[0] = box(0, 600);
    f.dets[1] = box(0, 110);
    assert(update(&f, 40) == 1);
    image_tracking_snapshot(&s);
    assert(s.active && s.selected == 1 && s.track_id == 1);
    assert(s.object_ids[0] == 2 && s.object_ids[1] == 1);
    /* Brief detector dropout recovers nearby, using the same track ID. */
    f.count = 0;
    assert(update(&f, 40) == -1);
    f.count = 1;
    f.dets[0] = box(0, 120);
    assert(update(&f, 40) == 0);
    /* A different class cannot steal the track. */
    f.dets[0] = box(67, 120);
    assert(update(&f, 40) == -1);
    f.dets[0] = box(0, 125);
    assert(update(&f, 40) == 0);
    /* Two indistinguishable candidates pause association. */
    f.count = 2;
    f.dets[0] = f.dets[1] = box(0, 130);
    assert(update(&f, 40) == -1);
    f.count = 1;
    f.dets[0] = box(0, 130);
    assert(update(&f, 40) == 0);
    /* Selection from an earlier displayed frame associates to the current one. */
    unsigned long long old_seq = f.seq;
    f.dets[0] = box(0, 135);
    assert(update(&f, 40) == 0);
    assert(image_tracking_select(old_seq, 0) == 0);
    image_tracking_snapshot(&s);
    assert(s.track_id == 1 && s.selected == 0);
    /* Long gaps never silently reacquire a potentially different object. */
    assert(update(&f, 900) == -1);
    assert(image_tracking_select(old_seq, 0) == -1);
    assert(image_tracking_select(f.seq, 0) == 0);
    /* Out-of-order publication leaves the accepted snapshot intact. */
    yolo_result_t reordered = f;
    reordered.seq = 1;
    assert(image_tracking_update(&reordered, NULL, test_ms - 1, NULL, 0, NULL) == -1);
    image_tracking_snapshot(&s);
    assert(s.frame.seq == f.seq);
    /* Stop is independent of continued detection publication. */
    image_tracking_stop();
    assert(update(&f, 40) == -1);
    image_tracking_snapshot(&s);
    assert(!s.active && s.fresh);
    /* A restarted capture clears pending identities and old selection history. */
    assert(image_tracking_select(f.seq, 0) == 0);
    old_seq = f.seq;
    f.seq = 0;
    assert(update(&f, 40) == -1);
    assert(image_tracking_select(old_seq, 0) == -1);
    image_tracking_snapshot(&s);
    assert(!s.active);
    test_ms += 1100;
    image_tracking_snapshot(&s);
    assert(!s.fresh && s.selected == -1);
    puts("PASS: image tracking association, history, loss, stop, session and freshness");
    return 0;
}
