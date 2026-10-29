/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cc -std=c11 -Iinclude tests/appearance_tracking_test.c source/control/image_tracking.c
 *    source/control/appearance_features.c -pthread -lm -o /tmp/appearance_tracking_test */
#define _POSIX_C_SOURCE 200809L
#include "control/image_tracking.h"
#include "control/appearance_features.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define W 320
#define H 180
static unsigned char rgb[W * H * 3];
static unsigned long long test_ms = 10000;
int clock_gettime(clockid_t clock, struct timespec *out)
{
    assert(clock == CLOCK_MONOTONIC);
    out->tv_sec = test_ms / 1000;
    out->tv_nsec = test_ms % 1000 * 1000000;
    return 0;
}
static yolo_det_t box(int x)
{
    return (yolo_det_t){.cls = 0, .conf = .95f, .x1 = x, .y1 = 30, .x2 = x + 60, .y2 = 150};
}
static void paint(const yolo_det_t *b, int red, int green, int blue)
{
    for (int y = (int)b->y1; y < (int)b->y2; ++y)
        for (int x = (int)b->x1; x < (int)b->x2; ++x) {
            unsigned char *p = rgb + (y * W + x) * 3;
            // A repeatable texture makes the gradient channel nonempty too.
            int light = ((x / 4 + y / 4) & 1) ? 8 : 0;
            p[0] = red + light; p[1] = green + light; p[2] = blue + light;
        }
}
static int update(yolo_result_t *f, unsigned elapsed)
{
    test_ms += elapsed;
    ++f->seq;
    return image_tracking_update(f, NULL, test_ms, rgb, sizeof(rgb), NULL);
}
int main(void)
{
    yolo_result_t f = {.w = W, .h = H, .count = 2};
    f.dets[0] = box(20); f.dets[1] = box(230);
    paint(&f.dets[0], 200, 20, 20);
    paint(&f.dets[1], 20, 20, 200);
    appearance_feature_t red, blue, invalid;
    assert(!appearance_extract(rgb, sizeof(rgb), W, H, &f.dets[0], &red));
    assert(!appearance_extract(rgb, sizeof(rgb), W, H, &f.dets[1], &blue));
    double norm = 0;
    for (int i = 0; i < APPEARANCE_DIM; ++i) norm += red.values[i] * red.values[i];
    assert(fabs(norm - 1) < 1e-5);
    assert(appearance_similarity(&red, &red) > .9999);
    assert(appearance_similarity(&red, &blue) < .78);
    /* Same colors with different texture must not collapse to one descriptor. */
    for (int i = 0; i < 2; ++i) {
        const yolo_det_t *b = &f.dets[i];
        for (int y = (int)b->y1; y < (int)b->y2; ++y)
            for (int x = (int)b->x1; x < (int)b->x2; ++x) {
                int stripe = i ? (((x - (int)b->x1) / 4 + y / 4) & 1) : ((y / 4) & 1);
                unsigned char *p = rgb + (y * W + x) * 3;
                p[0] = stripe ? 200 : 20; p[1] = 20; p[2] = stripe ? 20 : 200;
            }
    }
    appearance_feature_t stripes, checker;
    assert(!appearance_extract(rgb, sizeof(rgb), W, H, &f.dets[0], &stripes));
    assert(!appearance_extract(rgb, sizeof(rgb), W, H, &f.dets[1], &checker));
    assert(appearance_similarity(&stripes, &checker) < .995);
    paint(&f.dets[0], 150, 15, 15);
    appearance_feature_t dim_red;
    assert(!appearance_extract(rgb, sizeof(rgb), W, H, &f.dets[0], &dim_red));
    assert(appearance_similarity(&red, &dim_red) > .95);
    paint(&f.dets[0], 200, 20, 20); paint(&f.dets[1], 20, 20, 200);
    assert(appearance_extract(rgb, sizeof(rgb) - 1, W, H, &f.dets[0], &invalid) == -1);
    assert(!invalid.valid);
    yolo_det_t bad = f.dets[0]; bad.x2 = NAN;
    assert(appearance_extract(rgb, sizeof(rgb), W, H, &bad, &invalid) == -1);
    bad = box(20); bad.x2 = bad.x1 + 5;
    assert(appearance_extract(rgb, sizeof(rgb), W, H, &bad, &invalid) == -1);

    assert(update(&f, 40) == -1);
    assert(!image_tracking_select(f.seq, 0));
    image_tracking_snapshot_t s;
    image_tracking_snapshot(&s);
    assert(s.reference_ready && s.features_available == 2 && s.appearance_similarity > .99);
    /* A same-class blue impostor takes the old position; red jumps beyond the
     * motion gate. Appearance must reject the nearer blue detection. */
    memset(rgb, 0, sizeof(rgb));
    f.dets[0] = box(20); f.dets[1] = box(160);
    paint(&f.dets[0], 20, 20, 200); paint(&f.dets[1], 200, 20, 20);
    assert(update(&f, 40) == -1);
    assert(update(&f, 40) == 1);
    image_tracking_snapshot(&s);
    assert(s.track_id == 1 && s.reidentifications == 1 && s.appearance_similarity > .94);
    assert(s.object_ids[s.selected] == s.track_id && s.object_ids[0] != s.object_ids[1]);
    /* A missed detection is followed by strong appearance reacquisition at a
     * new position after the legacy 800 ms motion timeout. */
    f.count = 0;
    assert(update(&f, 1000) == -1);
    f.count = 1; f.dets[0] = box(30);
    memset(rgb, 0, sizeof(rgb)); paint(&f.dets[0], 200, 20, 20);
    assert(update(&f, 40) == -1);
    assert(update(&f, 40) == 0);
    image_tracking_snapshot(&s);
    assert(s.track_id == 1 && s.reidentifications == 2);
    assert(s.object_ids[s.selected] == s.track_id);
    /* No RGB evidence may fall back to motion once appearance is enrolled. */
    test_ms += 40; ++f.seq;
    assert(image_tracking_update(&f, NULL, test_ms, NULL, 0, NULL) == -1);
    assert(update(&f, 40) == 0);
    /* Duplicate lookalike boxes are ambiguous, even with a high similarity. */
    f.count = 2; f.dets[1] = f.dets[0];
    assert(update(&f, 40) == -1);
    f.count = 1;
    assert(update(&f, 40) == 0);
    /* Wrong appearance never contaminates the immutable reference. */
    memset(rgb, 0, sizeof(rgb)); paint(&f.dets[0], 20, 20, 200);
    for (int i = 0; i < 10; ++i) assert(update(&f, 40) == -1);
    memset(rgb, 0, sizeof(rgb)); paint(&f.dets[0], 200, 20, 20);
    assert(update(&f, 40) == 0);
    /* Re-identification is bounded; a later similar object needs a new click. */
    assert(update(&f, 5100) == -1);
    image_tracking_snapshot(&s);
    assert(!s.reacquire_remaining_ms);
    assert(!image_tracking_select(f.seq, 0));
    image_tracking_snapshot(&s);
    assert(s.track_id > 1 && s.reference_ready);
    image_tracking_stop();
    image_tracking_snapshot(&s);
    assert(!s.active && !s.reference_ready && s.appearance_similarity < 0);
    /* Tiny boxes cannot silently start appearance-free tracking in production. */
    f.dets[0] = bad;
    assert(update(&f, 40) == -1);
    assert(image_tracking_select(f.seq, 0) == -1);
    /* A click enrolls a recent original-frame appearance even if a moving
     * object has left that position before the HTTP request arrives. */
    f.seq = 0; f.dets[0] = box(20);
    memset(rgb, 0, sizeof(rgb)); paint(&f.dets[0], 200, 20, 20);
    assert(update(&f, 40) == -1);
    unsigned long long original_seq = f.seq;
    f.dets[0] = box(220);
    memset(rgb, 0, sizeof(rgb)); paint(&f.dets[0], 200, 20, 20);
    assert(update(&f, 40) == -1);
    assert(!image_tracking_select(original_seq, 0));
    image_tracking_snapshot(&s);
    assert(s.reference_ready && s.active && s.selected == -1);
    assert(update(&f, 40) == 0);
    puts("PASS: descriptors, appearance veto, re-identification, ambiguity, reference protection and stop");
    return 0;
}
