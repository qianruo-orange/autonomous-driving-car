/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef APPEARANCE_FEATURES_H
#define APPEARANCE_FEATURES_H
#include "yolo/yolo_types.h"

/* Five spatial cells (whole crop + 2x2). Each contains 52 HSV/gray bins,
 * 10 rotation-invariant uniform-LBP bins and 9 unsigned gradient bins.
 * This is a CPU appearance descriptor, not a learned ReID embedding. */
#define APPEARANCE_DIM (5 * (52 + 10 + 9))
typedef struct {
    float values[APPEARANCE_DIM];
    int valid;
} appearance_feature_t;

/* RGB24, tightly packed, before any boxes/labels are painted. Invalid/tiny
 * crops return -1 and clear out. Samples a bounded 64x64 grid per detection. */
int appearance_extract(const unsigned char *rgb, size_t bytes, int width, int height,
                       const yolo_det_t *box, appearance_feature_t *out);
/* Cosine similarity of normalized descriptors, or -1 if unavailable. */
double appearance_similarity(const appearance_feature_t *, const appearance_feature_t *);
/* Slow adaptation. Caller must gate with the immutable enrollment reference. */
void appearance_blend(appearance_feature_t *out, const appearance_feature_t *, double alpha);
#endif
