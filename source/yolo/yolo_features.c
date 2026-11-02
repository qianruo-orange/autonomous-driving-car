/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "yolo/yolo_features.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

int yolo_features_supported(const rknn_tensor_attr *a)
{
    if (!a || a->n_dims != 4 || a->dims[0] != 1 ||
        (a->fmt != RKNN_TENSOR_NCHW && a->fmt != RKNN_TENSOR_NHWC)) return 0;
    unsigned c = a->fmt == RKNN_TENSOR_NCHW ? a->dims[1] : a->dims[3];
    unsigned h = a->fmt == RKNN_TENSOR_NCHW ? a->dims[2] : a->dims[1];
    unsigned w = a->fmt == RKNN_TENSOR_NCHW ? a->dims[3] : a->dims[2];
    if (c != YOLO_FEATURE_DIM || h == 0 || h > INT_MAX || w == 0 || w > INT_MAX) return 0;
    uint64_t pixels = (uint64_t)h * w;
    return pixels <= UINT32_MAX / YOLO_FEATURE_DIM &&
           pixels <= SIZE_MAX / sizeof(float) / YOLO_FEATURE_DIM &&
           pixels * YOLO_FEATURE_DIM == a->n_elems;
}

int yolo_features_extract(const float *fmap, const rknn_tensor_attr *a,
                          int input_w, int input_h, float scale_x, float scale_y,
                          int pad_x, int pad_y, yolo_det_t *dets, int count)
{
    if (dets)
        for (int i = 0; i < count && i < YOLO_MAX_DETS; ++i)
            memset(&dets[i].feature, 0, sizeof(dets[i].feature));
    if (!dets || count < 0 || count > YOLO_MAX_DETS || !fmap ||
        !yolo_features_supported(a) || input_w <= 0 || input_h <= 0 ||
        !isfinite(scale_x) || !isfinite(scale_y) || scale_x <= 0 || scale_y <= 0 ||
        pad_x < 0 || pad_y < 0 || pad_x >= input_w || pad_y >= input_h) return -1;

    int nchw = a->fmt == RKNN_TENSOR_NCHW;
    int h = (int)a->dims[nchw ? 2 : 1], w = (int)a->dims[nchw ? 3 : 2];
    size_t pixel_stride = nchw ? 1 : YOLO_FEATURE_DIM;
    size_t channel_stride = nchw ? (size_t)w * h : 1;
    int valid = 0;
    for (int i = 0; i < count; ++i) {
        yolo_det_t *d = &dets[i];
        if (!isfinite(d->x1) || !isfinite(d->y1) || !isfinite(d->x2) || !isfinite(d->y2) ||
            d->x2 <= d->x1 || d->y2 <= d->y1) continue;
        double x1 = fmax(0, fmin(input_w, (double)d->x1 * scale_x + pad_x));
        double y1 = fmax(0, fmin(input_h, (double)d->y1 * scale_y + pad_y));
        double x2 = fmax(0, fmin(input_w, (double)d->x2 * scale_x + pad_x));
        double y2 = fmax(0, fmin(input_h, (double)d->y2 * scale_y + pad_y));
        if (x2 <= x1 || y2 <= y1) continue;
        double dx = .05 * (x2 - x1), dy = .05 * (y2 - y1);
        x1 += dx; x2 -= dx; y1 += dy; y2 -= dy;

        /* Feature cells are sampled at their centers (align_corners=false).
         * Precompute the 16 spatial taps once, then reuse them per channel. */
        size_t positions[16][4];
        double weights[16][4];
        for (int gy = 0; gy < 4; ++gy) for (int gx = 0; gx < 4; ++gx) {
            double x = (x1 + (gx + .5) * (x2 - x1) / 4) * w / input_w - .5;
            double y = (y1 + (gy + .5) * (y2 - y1) / 4) * h / input_h - .5;
            x = fmax(0, fmin(w - 1, x)); y = fmax(0, fmin(h - 1, y));
            int ix = (int)x, iy = (int)y, jx = ix + (ix < w - 1), jy = iy + (iy < h - 1);
            double fx = x - ix, fy = y - iy;
            int k = gy * 4 + gx;
            positions[k][0] = ((size_t)iy * w + ix) * pixel_stride;
            positions[k][1] = ((size_t)iy * w + jx) * pixel_stride;
            positions[k][2] = ((size_t)jy * w + ix) * pixel_stride;
            positions[k][3] = ((size_t)jy * w + jx) * pixel_stride;
            weights[k][0] = (1 - fx) * (1 - fy); weights[k][1] = fx * (1 - fy);
            weights[k][2] = (1 - fx) * fy; weights[k][3] = fx * fy;
        }
        double pooled[YOLO_FEATURE_DIM], norm = 0;
        int finite = 1;
        for (int c = 0; c < YOLO_FEATURE_DIM && finite; ++c) {
            double sum = 0;
            for (int k = 0; k < 16 && finite; ++k) for (int t = 0; t < 4; ++t) {
                if (weights[k][t] == 0) continue;
                float value = fmap[(size_t)c * channel_stride + positions[k][t]];
                if (!isfinite(value)) { finite = 0; break; }
                sum += value * weights[k][t];
            }
            pooled[c] = sum / 16;
            norm += pooled[c] * pooled[c];
        }
        if (!finite || !isfinite(norm) || norm <= 1e-12) continue;
        norm = sqrt(norm);
        for (int c = 0; c < YOLO_FEATURE_DIM; ++c)
            d->feature.values[c] = (float)(pooled[c] / norm);
        d->feature.valid = 1;
        ++valid;
    }
    return valid;
}

double yolo_feature_similarity(const yolo_feature_t *a, const yolo_feature_t *b)
{
    if (!a || !b || !a->valid || !b->valid) return -1;
    double dot = 0, aa = 0, bb = 0;
    for (int c = 0; c < YOLO_FEATURE_DIM; ++c) {
        double av = a->values[c], bv = b->values[c];
        if (!isfinite(av) || !isfinite(bv)) return -1;
        dot += av * bv; aa += av * av; bb += bv * bv;
    }
    if (aa <= 1e-12 || bb <= 1e-12) return -1;
    return fmax(-1, fmin(1, dot / (sqrt(aa) * sqrt(bb))));
}

static int normalize_feature(yolo_feature_t *f)
{
    double norm = 0;
    for (int c = 0; c < YOLO_FEATURE_DIM; ++c) {
        if (!isfinite(f->values[c])) { memset(f, 0, sizeof(*f)); return -1; }
        norm += (double)f->values[c] * f->values[c];
    }
    if (norm <= 1e-12) { memset(f, 0, sizeof(*f)); return -1; }
    norm = sqrt(norm);
    for (int c = 0; c < YOLO_FEATURE_DIM; ++c) f->values[c] /= norm;
    f->valid = 1;
    return 0;
}

void yolo_feature_blend(yolo_feature_t *out, const yolo_feature_t *sample, double alpha)
{
    if (!out || !sample || !sample->valid || !isfinite(alpha) || alpha <= 0 || alpha > 1) return;
    yolo_feature_t result = *sample;
    if (normalize_feature(&result) != 0) return;
    yolo_feature_t previous = *out;
    if (out->valid && normalize_feature(&previous) == 0)
        for (int c = 0; c < YOLO_FEATURE_DIM; ++c)
            result.values[c] = (float)((1 - alpha) * previous.values[c] + alpha * result.values[c]);
    normalize_feature(&result);
    *out = result;
}
