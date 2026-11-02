/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef YOLO_FEATURES_H
#define YOLO_FEATURES_H

#include "yolo/rknn_api.h"
#include "yolo/yolo_types.h"

/* One contiguous NCHW or NHWC feature map, batch 1, 128 channels. The native
 * tensor type may be quantized; extract requires SDK want_float=1 output. */
int yolo_features_supported(const rknn_tensor_attr *attr);

/* Extract one L2-normalized descriptor per final, unsmoothed detection.
 * Boxes are in original-image coordinates. scale/pad are the exact letterbox
 * geometry that produced this inference; input_w/h describe the model input.
 * Samples a fixed 4x4 bilinear grid after trimming 5% of each ROI edge.
 * fmap must contain attr->n_elems dequantized floats and remain alive until
 * return. Features are copied into dets; no SDK buffer is retained.
 * Returns the number of valid features, or -1 for an invalid map/geometry/count.
 * A zero/empty ROI or sampled NaN/Inf clears that detection's feature. Existing
 * features are cleared first, for at most YOLO_MAX_DETS entries. */
int yolo_features_extract(const float *fmap, const rknn_tensor_attr *attr,
                          int input_w, int input_h, float scale_x, float scale_y,
                          int pad_x, int pad_y, yolo_det_t *dets, int count);

/* Cosine in [-1,1]; -1 also indicates an unavailable/invalid descriptor. */
double yolo_feature_similarity(const yolo_feature_t *a, const yolo_feature_t *b);
/* Blend normalized descriptors and renormalize. Invalid sample/alpha leaves
 * out unchanged; a missing/invalid out initializes from the sample. */
void yolo_feature_blend(yolo_feature_t *out, const yolo_feature_t *sample, double alpha);

#endif
