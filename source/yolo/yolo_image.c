/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * yolo_image.c — Uncompressed pixel conversion and RGB image scaling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "yolo/yolo_image.h"

void yolo_yuyv_to_rgb(const unsigned char *src, int w, int h, unsigned char *dst)
{
    int npix = w * h / 2;
    for (int i = 0; i < npix; i++) {
        int y0 = src[0], u = src[1] - 128, y1 = src[2], v = src[3] - 128;
        src += 4;
        for (int k = 0; k < 2; k++) {
            int y = k ? y1 : y0;
            /* BT.601 定点系数（×1/1024 ≈ 1.402/0.344/0.714/1.772）：免浮点。
               UVC 相机 YUYV 为 BT.601；下游 OpenCV RGB2YUV_I420 与编码器
               VUI 声明也是 601，全链一致 */
            int r = y + ((1436 * v) >> 10);
            int g = y - ((352 * u + 731 * v) >> 10);
            int b = y + ((1815 * u) >> 10);
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            *dst++ = (unsigned char)r;
            *dst++ = (unsigned char)g;
            *dst++ = (unsigned char)b;
        }
    }
}

#if defined(__aarch64__)
static uint8x16_t jpeg_rgb_channel(uint8x16_t y, int16x8_t delta)
{
    int16x8_t lo = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(y)));
    int16x8_t hi = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(y)));
    return vcombine_u8(vqmovun_s16(vaddq_s16(lo, vzip1q_s16(delta, delta))),
                       vqmovun_s16(vaddq_s16(hi, vzip2q_s16(delta, delta))));
}
#endif

void yolo_nv12_to_rgb_full(const unsigned char *y, const unsigned char *uv,
                            int stride, int w, int h, unsigned char *rgb)
{
    for (int row = 0; row < h; ++row) {
        const unsigned char *luma = y + (size_t)row * stride;
        const unsigned char *chroma = uv + (size_t)(row / 2) * stride;
        unsigned char *dst = rgb + (size_t)row * w * 3;
        int x = 0;
#if defined(__aarch64__)
        for (; x + 16 <= w; x += 16) {
            uint8x8x2_t pair = vld2_u8(chroma + x);
            int16x8_t u = vreinterpretq_s16_u16(vsubl_u8(pair.val[0], vdup_n_u8(128)));
            int16x8_t v = vreinterpretq_s16_u16(vsubl_u8(pair.val[1], vdup_n_u8(128)));
            int16x8_t r = vcombine_s16(vrshrn_n_s32(vmull_n_s16(vget_low_s16(v), 1436), 10),
                                       vrshrn_n_s32(vmull_n_s16(vget_high_s16(v), 1436), 10));
            int16x8_t b = vcombine_s16(vrshrn_n_s32(vmull_n_s16(vget_low_s16(u), 1815), 10),
                                       vrshrn_n_s32(vmull_n_s16(vget_high_s16(u), 1815), 10));
            int32x4_t gl = vmlal_n_s16(vmull_n_s16(vget_low_s16(u), 352), vget_low_s16(v), 731);
            int32x4_t gh = vmlal_n_s16(vmull_n_s16(vget_high_s16(u), 352), vget_high_s16(v), 731);
            int16x8_t g = vnegq_s16(vcombine_s16(vrshrn_n_s32(gl, 10), vrshrn_n_s32(gh, 10)));
            uint8x16_t yy = vld1q_u8(luma + x);
            uint8x16x3_t output = {{jpeg_rgb_channel(yy, r), jpeg_rgb_channel(yy, g), jpeg_rgb_channel(yy, b)}};
            vst3q_u8(dst + (size_t)x * 3, output);
        }
#endif
        for (; x < w; ++x) {
            int u = chroma[x & ~1] - 128, v = chroma[(x & ~1) + 1] - 128;
            int r = luma[x] + ((1436 * v + 512) >> 10);
            int g = luma[x] - ((352 * u + 731 * v + 512) >> 10);
            int b = luma[x] + ((1815 * u + 512) >> 10);
            dst[x * 3] = (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r);
            dst[x * 3 + 1] = (unsigned char)(g < 0 ? 0 : g > 255 ? 255 : g);
            dst[x * 3 + 2] = (unsigned char)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
    }
}

void yolo_rgb_to_nv12(const unsigned char *rgb, int w, int h, unsigned char *nv12)
{
    unsigned char *Y = nv12;
    unsigned char *UV = nv12 + (size_t)w * h;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = rgb + (size_t)y * w * 3;
        unsigned char *Yrow = Y + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            const unsigned char *p = row + x * 3;
            int r = p[0], g = p[1], b = p[2];
            /* BT.601 limited-range 整数系数：与快路径 OpenCV RGB2YUV_I420
               及编码器 VUI 声明（BT.601）一致——本函数是快路径的兜底，
               两者系数必须相同，否则兜底帧会突变色 */
            Yrow[x] = (unsigned char)((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
            if ((y & 1) == 0 && (x & 1) == 0) {
                unsigned char *uv = UV + (size_t)(y / 2) * w + x;
                uv[0] = (unsigned char)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
                uv[1] = (unsigned char)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
            }
        }
    }
}
