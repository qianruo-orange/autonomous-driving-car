/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "control/appearance_features.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

#define GRID 64
#define BINS 71

static int normalize(appearance_feature_t *feature)
{
    feature->valid = 0;
    double norm = 0;
    for (int i = 0; i < APPEARANCE_DIM; ++i) norm += feature->values[i] * feature->values[i];
    if (!isfinite(norm) || norm < 1e-12) return -1;
    norm = sqrt(norm);
    for (int i = 0; i < APPEARANCE_DIM; ++i) feature->values[i] /= norm;
    feature->valid = 1;
    return 0;
}

int appearance_extract(const unsigned char *rgb, size_t bytes, int width, int height,
                       const yolo_det_t *box, appearance_feature_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!rgb || !box || width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / 3 / (size_t)height ||
        bytes < (size_t)width * height * 3 || !isfinite(box->x1) || !isfinite(box->y1) ||
        !isfinite(box->x2) || !isfinite(box->y2) || box->x1 < 0 || box->y1 < 0 ||
        box->x2 > width || box->y2 > height || box->x2 - box->x1 < 12 ||
        box->y2 - box->y1 < 12) return -1;

    unsigned char gray[GRID][GRID];
    unsigned char colors[GRID][GRID];
    // Trim 5% off the crop to reduce background and unstable detector edges.
    double x0 = box->x1 + .05 * (box->x2 - box->x1);
    double y0 = box->y1 + .05 * (box->y2 - box->y1);
    double w = .9 * (box->x2 - box->x1), h = .9 * (box->y2 - box->y1);
    for (int y = 0; y < GRID; ++y) for (int x = 0; x < GRID; ++x) {
        int sx = (int)(x0 + (x + .5) * w / GRID);
        int sy = (int)(y0 + (y + .5) * h / GRID);
        const unsigned char *p = rgb + ((size_t)sy * width + sx) * 3;
        double r = p[0], g = p[1], b = p[2];
        double hi = r > g ? r : g, lo = r < g ? r : g;
        if (b > hi) hi = b;
        if (b < lo) lo = b;
        double delta = hi - lo;
        gray[y][x] = (unsigned char)((77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8);
        double saturation = hi > 0 ? delta / hi : 0;
        if (saturation < .15) colors[y][x] = 48 + gray[y][x] / 64;
        else {
            double hue = hi == r ? (g - b) / delta : hi == g ? 2 + (b - r) / delta : 4 + (r - g) / delta;
            if (hue < 0) hue += 6;
            int hb = (int)(hue * 2), sb = (int)(saturation * 4);
            if (hb > 11) hb = 11;
            if (sb > 3) sb = 3;
            colors[y][x] = hb * 4 + sb;
        }
    }
    for (int y = 1; y < GRID - 1; ++y) for (int x = 1; x < GRID - 1; ++x) {
        int cell = 1 + (y >= GRID / 2) * 2 + (x >= GRID / 2);
        unsigned code = 0;
        const int dx[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
        const int dy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
        for (int k = 0; k < 8; ++k)
            if (gray[y + dy[k]][x + dx[k]] >= gray[y][x]) code |= 1u << k;
        unsigned rotated = ((code << 1) | (code >> 7)) & 255;
        int transitions = __builtin_popcount(code ^ rotated);
        int lbp = transitions <= 2 ? __builtin_popcount(code) : 9;
        double gx = (int)gray[y][x + 1] - gray[y][x - 1];
        double gy = (int)gray[y + 1][x] - gray[y - 1][x];
        // Compare slopes at 20/40/60/80 degrees instead of evaluating atan2
        // per sample. L1 gradient magnitude avoids another per-pixel sqrt.
        double ax = fabs(gx), ay = fabs(gy);
        int direction = ay < ax * .3639702343 ? 0 : ay < ax * .8390996312 ? 1 :
                        ay < ax * 1.7320508076 ? 2 : ay < ax * 5.6712818196 ? 3 : 4;
        if (gx * gy < 0) direction = 8 - direction;
        for (int c = 0; c < 2; ++c) {
            int off = (c ? cell : 0) * BINS;
            out->values[off + colors[y][x]] += 1;
            out->values[off + 52 + lbp] += 1;
            out->values[off + 62 + direction] += (float)(ax + ay);
        }
    }
    // Hellinger-normalize each histogram before weighting its contribution.
    const int starts[] = {0, 52, 62}, ends[] = {52, 62, 71};
    const double weights[] = {.60, .25, .15};
    for (int c = 0; c < 5; ++c) for (int group = 0; group < 3; ++group) {
        double sum = 0;
        for (int k = starts[group]; k < ends[group]; ++k) sum += out->values[c * BINS + k];
        if (sum <= 0) continue;
        double weight = sqrt(weights[group] * (c == 0 ? .5 : .125));
        for (int k = starts[group]; k < ends[group]; ++k)
            out->values[c * BINS + k] = (float)(weight * sqrt(out->values[c * BINS + k] / sum));
    }
    return normalize(out);
}

double appearance_similarity(const appearance_feature_t *a, const appearance_feature_t *b)
{
    if (!a || !b || !a->valid || !b->valid) return -1;
    double dot = 0;
    for (int i = 0; i < APPEARANCE_DIM; ++i) dot += a->values[i] * b->values[i];
    return isfinite(dot) ? fmax(0, fmin(1, dot)) : -1;
}

void appearance_blend(appearance_feature_t *out, const appearance_feature_t *sample, double alpha)
{
    if (!out || !sample || !sample->valid || !isfinite(alpha) || alpha <= 0 || alpha > 1) return;
    if (!out->valid) { *out = *sample; return; }
    for (int i = 0; i < APPEARANCE_DIM; ++i)
        out->values[i] = (float)((1 - alpha) * out->values[i] + alpha * sample->values[i]);
    normalize(out);
}
