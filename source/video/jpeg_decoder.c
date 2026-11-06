/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * JPEG -> NV12，libjpeg-turbo 软件解码。
 *
 * 为什么不用 V4L2 硬解（rockchip-jpegd / VDPU720）：
 *   同一张 JPEG 连续解码多次，结果互不相同 —— 8x8 块内第 0 行总是精确
 *   正确，第 1~7 行的误差每次都不一样（0.35 递增到 2.29），同一帧连解
 *   6 次得到 6 个不同 md5；只含水平条纹的输入 100% 逐字节一致，只含垂直
 *   条纹的输入则有整块残缺。画面上表现为块状灰斑、边缘断裂。这是 IP 的
 *   块写回问题，不是 IDCT 精度，无法通过调参数绕过，所以这里固定软解。
 *
 * 软解与 ffmpeg 参考逐像素一致（mean|d| 0.04，|dY|>32 像素为 0），
 * 720p 单帧 4~6ms，10~30fps 下 CPU 开销可忽略。
 *
 * 输出仍是 CMA 上的 NV12 环形缓冲，带 dma-buf fd，供 RGA 硬件 letterbox
 * 直接读取，下游通路与硬解时完全一致。
 */
#define _GNU_SOURCE
#include "video/jpeg_decoder.h"
#include "core/log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <jpeglib.h>
#include <linux/dma-heap.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* NV12 环形缓冲：解码结果按帧轮转，环深远大于在途帧数，避免与 worker 竞争 */
#define NV12_RING 8

typedef struct { int fd; unsigned char *va; size_t len; } nv12_slot_t;

/* libjpeg 默认的错误处理会 exit()，必须 longjmp 回来 */
typedef struct { struct jpeg_error_mgr pub; jmp_buf jump; } sw_error_t;

/* 一次 jpeg_read_raw_data 最多读出的亮度行数（4:2:0 为 16，4:2:2 为 8） */
#define SW_ICMCU_ROWS (DCTSIZE * 2)

struct jpeg_decoder {
    int width, height;
    nv12_slot_t nv12[NV12_RING];
    int nv12_next, nv12_w, nv12_h;
    /* cinfo 跨帧复用，避免每帧 create/destroy */
    int sw_ready;
    struct jpeg_decompress_struct cinfo;
    sw_error_t sw_err;
    unsigned char *sw_buf;   /* Cb/Cr 行缓冲 */
    size_t sw_cap;
};

static atomic_ullong decoded_frames, decode_errors;

static void sw_error_exit(j_common_ptr info)
{
    sw_error_t *err = (sw_error_t *)info->err;
    longjmp(err->jump, 1);
}

static void nv12_ring_free(jpeg_decoder_t *d)
{
    for (int i = 0; i < NV12_RING; ++i) {
        if (d->nv12[i].va) munmap(d->nv12[i].va, d->nv12[i].len);
        if (d->nv12[i].fd > 0) close(d->nv12[i].fd);
        d->nv12[i] = (nv12_slot_t){0};
    }
    d->nv12_next = 0;
    d->nv12_w = d->nv12_h = 0;
}

/* 按需分配（尺寸变化时重建）；CMA 连续内存是 RGA 的硬性要求 */
static int nv12_ring_reserve(jpeg_decoder_t *d, int w, int h)
{
    if (d->nv12_w == w && d->nv12_h == h && d->nv12[0].fd > 0) return 0;
    nv12_ring_free(d);
    size_t len = (size_t)w * h * 3 / 2;
    int heap = open("/dev/dma_heap/default_cma_region", O_RDWR | O_CLOEXEC);
    if (heap < 0) return -1;
    int failed = 0;
    for (int i = 0; i < NV12_RING; ++i) {
        struct dma_heap_allocation_data allocation = {.len = len, .fd_flags = O_RDWR | O_CLOEXEC};
        if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation)) { failed = 1; break; }
        void *va = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, (int)allocation.fd, 0);
        if (va == MAP_FAILED) { close((int)allocation.fd); failed = 1; break; }
        d->nv12[i] = (nv12_slot_t){.fd = (int)allocation.fd, .va = va, .len = len};
    }
    int saved = errno;
    close(heap);
    errno = saved;
    if (failed) { nv12_ring_free(d); return -1; }
    d->nv12_w = w;
    d->nv12_h = h;
    return 0;
}

/* 软件解码：jpeg_read_raw_data 取原生分量，不做色度上采样。
   相机 MJPEG 常见 4:2:2（h2v1），也有 4:2:0 的型号，两条都支持：
     4:2:2  色度已是半宽，垂直方向每 2 行平均成 1 行 UV
     4:2:0  色度已是半宽半高，直接逐行搬
   输出 NV12，Y 行距 = w，UV 交错行距 = w。 */
static int sw_decode_nv12(jpeg_decoder_t *d, const unsigned char *jpeg, size_t length,
                          int w, int h, unsigned char *dst)
{
    if (w % 16 || h % 16) { errno = EINVAL; return -1; }
    if (setjmp(d->sw_err.jump)) { jpeg_abort_decompress(&d->cinfo); errno = EIO; return -1; }
    struct jpeg_decompress_struct *cinfo = &d->cinfo;
    jpeg_mem_src(cinfo, jpeg, (unsigned long)length);
    if (jpeg_read_header(cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_abort_decompress(cinfo);
        errno = EIO;
        return -1;
    }
    /* 只接受 YCbCr 4:2:2 / 4:2:0：色度水平 2 合 1，垂直采样 1 或 2 */
    if (cinfo->num_components != 3 ||
        cinfo->comp_info[0].h_samp_factor != 2 || cinfo->comp_info[1].h_samp_factor != 1 ||
        cinfo->comp_info[2].h_samp_factor != 1 ||
        cinfo->comp_info[0].v_samp_factor != cinfo->comp_info[1].v_samp_factor ||
        cinfo->comp_info[1].v_samp_factor != cinfo->comp_info[2].v_samp_factor ||
        cinfo->comp_info[0].v_samp_factor > 2 || cinfo->comp_info[0].v_samp_factor < 1) {
        jpeg_abort_decompress(cinfo);
        errno = EINVAL;
        return -1;
    }
    int vs = cinfo->comp_info[0].v_samp_factor;   /* 2 = 4:2:0，1 = 4:2:2 */
    cinfo->out_color_space = JCS_YCbCr;
    cinfo->raw_data_out = TRUE;
    cinfo->dct_method = JDCT_ISLOW;
    jpeg_start_decompress(cinfo);
    if ((int)cinfo->output_width != w || (int)cinfo->output_height != h) {
        jpeg_abort_decompress(cinfo);
        errno = EINVAL;
        return -1;
    }
    int cw = (int)cinfo->comp_info[1].downsampled_width;   /* 亮度宽的一半 */
    int ch = (int)cinfo->comp_info[1].downsampled_height;  /* 亮度高 / vs */
    if (cw != w / 2 || ch != h / vs) { jpeg_abort_decompress(cinfo); errno = EINVAL; return -1; }

    int rows_per = (int)cinfo->max_v_samp_factor * DCTSIZE;   /* 每次读出的亮度行数 */
    size_t need = (size_t)cw * (size_t)rows_per * 2;          /* Cb + Cr 行缓冲 */
    if (need > d->sw_cap) {
        unsigned char *grown = realloc(d->sw_buf, need);
        if (!grown) { jpeg_abort_decompress(cinfo); errno = ENOMEM; return -1; }
        d->sw_buf = grown;
        d->sw_cap = need;
    }
    unsigned char *cb = d->sw_buf, *cr = d->sw_buf + (size_t)cw * rows_per;
    JSAMPROW yrows[SW_ICMCU_ROWS], cbrows[SW_ICMCU_ROWS], crrows[SW_ICMCU_ROWS];
    JSAMPARRAY planes[3] = { yrows, cbrows, crrows };
    for (int i = 0; i < rows_per; ++i) {
        cbrows[i] = (JSAMPROW)(cb + (size_t)i * cw);
        crrows[i] = (JSAMPROW)(cr + (size_t)i * cw);
    }
    unsigned char *plane_uv = dst + (size_t)w * h;
    int y = 0;
    while (y < h) {
        int rows = rows_per;
        if (y + rows > h) rows = h - y;
        for (int i = 0; i < rows; ++i) yrows[i] = (JSAMPROW)(dst + (size_t)(y + i) * w);
        if (!jpeg_read_raw_data(cinfo, planes, rows_per)) {
            jpeg_abort_decompress(cinfo);
            errno = EIO;
            return -1;
        }
        int crow = rows / vs;                 /* 本次拿到的色度行数 */
        if (vs == 1) {
            /* 4:2:2：色度已是半宽，垂直 2 行平均成 1 行 UV */
            for (int i = 0; i + 1 < crow; i += 2) {
                unsigned char *line = plane_uv + (size_t)(y / 2 + i / 2) * w;
                const unsigned char *s0b = cbrows[i], *s0r = crrows[i];
                const unsigned char *s1b = cbrows[i + 1], *s1r = crrows[i + 1];
                for (int x = 0; x < cw; ++x) {
                    line[x * 2] = (unsigned char)((s0b[x] + s1b[x] + 1) >> 1);
                    line[x * 2 + 1] = (unsigned char)((s0r[x] + s1r[x] + 1) >> 1);
                }
            }
        } else {
            /* 4:2:0：色度已是半宽半高，逐行直接搬（每 i 对应 UV 的一行） */
            for (int i = 0; i < crow; ++i) {
                unsigned char *line = plane_uv + (size_t)(y / 2 + i) * w;
                const unsigned char *s0b = cbrows[i], *s0r = crrows[i];
                for (int x = 0; x < cw; ++x) {
                    line[x * 2] = s0b[x];
                    line[x * 2 + 1] = s0r[x];
                }
            }
        }
        y += rows;
    }
    jpeg_finish_decompress(cinfo);
    return 0;
}

jpeg_decoder_t *jpeg_decoder_open(void)
{
    jpeg_decoder_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->cinfo.err = jpeg_std_error(&d->sw_err.pub);
    d->sw_err.pub.error_exit = sw_error_exit;
    if (setjmp(d->sw_err.jump)) {
        LOG_ERROR("jpeg: libjpeg init failed");
        free(d);
        return NULL;
    }
    jpeg_create_decompress(&d->cinfo);
    d->sw_ready = 1;
    LOG_INFO("jpeg: software decoder (libjpeg-turbo)");
    return d;
}

void jpeg_decoder_close(jpeg_decoder_t *d)
{
    if (!d) return;
    nv12_ring_free(d);
    if (d->sw_ready) jpeg_destroy_decompress(&d->cinfo);
    free(d->sw_buf);
    free(d);
}

/* libjpeg-turbo 是硬依赖（CMakeLists 里 REQUIRED），能编译出来就可用 */
int jpeg_decoder_available(void)
{
    return 1;
}

void jpeg_decoder_stats(unsigned long long *frames, unsigned long long *errors)
{
    if (frames) *frames = atomic_load(&decoded_frames);
    if (errors) *errors = atomic_load(&decode_errors);
}

int jpeg_decoder_nv12(jpeg_decoder_t *d, const unsigned char *jpeg, size_t length,
                      int w, int h, int *nv12_fd, unsigned char *copy_to)
{
    if (!d || !jpeg || !nv12_fd || !d->sw_ready || length < 4 ||
        w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
        jpeg[0] != 0xff || jpeg[1] != 0xd8) return -1;

    int rc = nv12_ring_reserve(d, w, h);
    nv12_slot_t *slot = NULL;
    if (!rc) {
        slot = &d->nv12[d->nv12_next];
        d->nv12_next = (d->nv12_next + 1) % NV12_RING;
        rc = sw_decode_nv12(d, jpeg, length, w, h, slot->va);
    }
    if (!rc && copy_to) memcpy(copy_to, slot->va, (size_t)w * h * 3 / 2);
    if (!rc) {
        *nv12_fd = slot->fd;
        d->width = w;
        d->height = h;
        atomic_fetch_add(&decoded_frames, 1);
        return 0;
    }
    int saved = errno;
    unsigned long long errors = atomic_fetch_add(&decode_errors, 1) + 1;
    if (errors <= 3 || errors % 100 == 0)
        LOG_ERROR("jpeg: software decode failed: %s", strerror(saved));
    errno = saved;
    return -1;
}
