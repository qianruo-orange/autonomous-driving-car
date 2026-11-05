/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * JPEG decode to NV12.
 *
 * 默认走 libjpeg-turbo 软件解码：板载 rkjpegd 硬件解码器在 RK3576 上
 * 亮度块内后半段丢细节（同帧对比：块内第 0 行误差 0.10，第 7 行 3.78，
 * 色度完全正确），成片因此布满沿边缘的亮点/暗点。软件解码 720p 约
 * 4~6ms，10~30fps 下 CPU 开销可忽略。
 *
 * JPEG_DECODE=hw 环境变量可切回 V4L2 硬件通路（保留以备上游修好）。
 */
#define _GNU_SOURCE
#include "video/jpeg_decoder.h"
#include "core/log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <jpeglib.h>
#include <linux/videodev2.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <setjmp.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CAPTURE_BUFFERS 2
#define DECODE_TIMEOUT_MS 500

typedef struct { void *data; size_t length; int fd; } mapped_buffer_t;

/* NV12 环形缓冲（CMA dma-buf）：解码结果按帧轮转，供 RGA 直接读。
   环深 8 远大于在途帧数，避免与 worker 竞争。 */
#define NV12_RING 8
typedef struct { int fd; unsigned char *va; size_t len; } nv12_slot_t;

/* libjpeg 错误处理：默认实现会 exit()，这里必须 longjmp 回来 */
typedef struct { struct jpeg_error_mgr pub; jmp_buf jump; } sw_error_t;

/* 软件解码的 iMCU 行缓冲（4:2:0，最大 16 行亮度 + 8 行色度） */
#define SW_MAX_H 8192
#define SW_ICMCU_ROWS (DCTSIZE * 2)

struct jpeg_decoder {
    int fd, width, height, output_on, capture_on, hardware;
    char device[32];
    mapped_buffer_t coded, raw[CAPTURE_BUFFERS];
    struct v4l2_pix_format_mplane format;
    nv12_slot_t nv12[NV12_RING];
    int nv12_next, nv12_w, nv12_h;
    /* 软件解码器：cinfo 跨帧复用，避免每帧 create/destroy */
    int sw_ready;
    struct jpeg_decompress_struct cinfo;
    sw_error_t sw_err;
    unsigned char *sw_buf;
    size_t sw_cap;
};
static atomic_ullong decoded_frames, decode_errors;

static int xioctl(int fd, unsigned long request, void *value)
{
    int rc;
    do rc = ioctl(fd, request, value); while (rc < 0 && errno == EINTR);
    return rc;
}
static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void sw_error_exit(j_common_ptr info)
{
    sw_error_t *err = (sw_error_t *)info->err;
    longjmp(err->jump, 1);
}
static int open_device(char *path, size_t size)
{
    for (int i = 0; i < 64; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "/dev/video%d", i);
        int fd = open(name, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct v4l2_capability cap = {0};
        if (!xioctl(fd, VIDIOC_QUERYCAP, &cap) &&
            !strcmp((char *)cap.driver, "rockchip-jpegd") &&
            (cap.device_caps & V4L2_CAP_VIDEO_M2M_MPLANE) &&
            (cap.device_caps & V4L2_CAP_STREAMING)) {
            snprintf(path, size, "%s", name);
            return fd;
        }
        close(fd);
    }
    errno = ENODEV;
    return -1;
}
int jpeg_decoder_available(void)
{
    /* 软件解码（libjpeg-turbo）恒可用；只有在强制硬件时才探测 V4L2 设备 */
    const char *mode = getenv("JPEG_DECODE");
    if (!mode || (strcmp(mode, "hw") && strcmp(mode, "hardware"))) return 1;
    char path[32];
    int fd = open_device(path, sizeof(path));
    if (fd < 0) return 0;
    close(fd);
    return 1;
}
void jpeg_decoder_stats(unsigned long long *frames, unsigned long long *errors)
{
    if (frames) *frames = atomic_load(&decoded_frames);
    if (errors) *errors = atomic_load(&decode_errors);
}
static void nv12_ring_free(jpeg_decoder_t *d);

static void release_buffers(jpeg_decoder_t *d)
{
    /* NV12 环不随解码失败释放：尺寸由 nv12_ring_reserve 校验，
       保留下来可避免每次重试都重新分配 8 个 CMA dma-buf */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (d->capture_on) (void)xioctl(d->fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (d->output_on) (void)xioctl(d->fd, VIDIOC_STREAMOFF, &type);
    if (d->coded.data) munmap(d->coded.data, d->coded.length);
    for (int i = 0; i < CAPTURE_BUFFERS; ++i)
        if (d->raw[i].data) {
            munmap(d->raw[i].data, d->raw[i].length);
            close(d->raw[i].fd);
        }
    if (d->fd >= 0) close(d->fd);
    d->fd = -1;
    d->coded = (mapped_buffer_t){0};
    memset(d->raw, 0, sizeof(d->raw));
    d->output_on = d->capture_on = d->width = d->height = 0;
}
jpeg_decoder_t *jpeg_decoder_open(void)
{
    jpeg_decoder_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->fd = -1;
    const char *mode = getenv("JPEG_DECODE");
    int want_hw = mode && (!strcmp(mode, "hw") || !strcmp(mode, "hardware"));
    if (!want_hw) {
        d->cinfo.err = jpeg_std_error(&d->sw_err.pub);
        d->sw_err.pub.error_exit = sw_error_exit;
        if (!setjmp(d->sw_err.jump)) {
            jpeg_create_decompress(&d->cinfo);
            d->sw_ready = 1;
            LOG_INFO("jpeg: software decoder (libjpeg-turbo), hardware path disabled");
            return d;
        }
        LOG_ERROR("jpeg: libjpeg init failed, falling back to hardware decoder");
        d->sw_ready = 0;
    }
    d->fd = open_device(d->device, sizeof(d->device));
    if (d->fd < 0) { free(d); return NULL; }
    d->hardware = 1;
    LOG_INFO("jpeg: hardware decoder %s (rockchip-jpegd)", d->device);
    return d;
}
void jpeg_decoder_close(jpeg_decoder_t *d)
{
    if (!d) return;
    nv12_ring_free(d);
    release_buffers(d);
    if (d->sw_ready) jpeg_destroy_decompress(&d->cinfo);
    free(d->sw_buf);
    free(d);
}
static int map_queue(jpeg_decoder_t *d, enum v4l2_buf_type type,
                      mapped_buffer_t *buffers, unsigned count)
{
    struct v4l2_requestbuffers request = {.type = type, .memory = V4L2_MEMORY_MMAP, .count = count};
    if (xioctl(d->fd, VIDIOC_REQBUFS, &request) || request.count < count) return -1;
    for (unsigned i = 0; i < count; ++i) {
        struct v4l2_plane plane = {0};
        struct v4l2_buffer buffer = {.type = type, .memory = V4L2_MEMORY_MMAP,
                                    .index = i, .length = 1, .m.planes = &plane};
        if (xioctl(d->fd, VIDIOC_QUERYBUF, &buffer) || !plane.length) return -1;
        void *data = mmap(NULL, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                          d->fd, plane.m.mem_offset);
        if (data == MAP_FAILED) return -1;
        buffers[i] = (mapped_buffer_t){data, plane.length, -1};
    }
    return 0;
}
static int queue_buffer(jpeg_decoder_t *d, enum v4l2_buf_type type,
                        unsigned index, size_t length)
{
    struct v4l2_plane plane = {.bytesused = (uint32_t)length};
    int capture = type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (capture) {
        plane.m.fd = d->raw[index].fd;
        plane.length = (uint32_t)d->raw[index].length;
    }
    struct v4l2_buffer buffer = {.type = type,
                                .memory = capture ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP,
                                .index = index, .length = 1, .m.planes = &plane};
    return xioctl(d->fd, VIDIOC_QBUF, &buffer);
}
static int allocate_capture(jpeg_decoder_t *d)
{
    struct v4l2_requestbuffers request = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_DMABUF, .count = CAPTURE_BUFFERS};
    if (xioctl(d->fd, VIDIOC_REQBUFS, &request) || request.count < CAPTURE_BUFFERS) return -1;
    int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (heap < 0) return -1;
    int failed = 0;
    for (int i = 0; i < CAPTURE_BUFFERS; ++i) {
        struct dma_heap_allocation_data allocation = {.len = d->format.plane_fmt[0].sizeimage,
            .fd_flags = O_RDWR | O_CLOEXEC};
        if (xioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation)) { failed = 1; break; }
        void *data = mmap(NULL, allocation.len, PROT_READ | PROT_WRITE, MAP_SHARED, (int)allocation.fd, 0);
        if (data == MAP_FAILED) { close((int)allocation.fd); failed = 1; break; }
        d->raw[i] = (mapped_buffer_t){data, allocation.len, (int)allocation.fd};
    }
    int saved = errno;
    close(heap);
    errno = saved;
    return failed ? -1 : 0;
}
static int queue_jpeg(jpeg_decoder_t *d, const unsigned char *jpeg, size_t length)
{
    if (length > d->coded.length) { errno = EMSGSIZE; return -1; }
    memcpy(d->coded.data, jpeg, length);
    /* Hardware fetches in 16-byte units; never expose old trailing bytes. */
    size_t padding = d->coded.length - length;
    memset((char *)d->coded.data + length, 0, padding < 64 ? padding : 64);
    return queue_buffer(d, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, 0, length);
}
static int prepare(jpeg_decoder_t *d, const unsigned char *jpeg, size_t length, int w, int h)
{
    if (d->fd < 0) d->fd = open_device(d->device, sizeof(d->device));
    if (d->fd < 0) return -1;
    struct v4l2_format out = {.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE};
    out.fmt.pix_mp = (struct v4l2_pix_format_mplane){.width = w, .height = h,
        .pixelformat = V4L2_PIX_FMT_JPEG, .num_planes = 1};
    out.fmt.pix_mp.plane_fmt[0].sizeimage = (uint32_t)((size_t)w * h * 2 + 65536);
    if (xioctl(d->fd, VIDIOC_S_FMT, &out) ||
        map_queue(d, out.type, &d->coded, 1) || queue_jpeg(d, jpeg, length) ||
        xioctl(d->fd, VIDIOC_STREAMON, &out.type)) return -1;
    d->output_on = 1;
    struct v4l2_format raw = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    struct v4l2_selection crop = {.type = raw.type, .target = V4L2_SEL_TGT_COMPOSE};
    if (xioctl(d->fd, VIDIOC_G_FMT, &raw) || xioctl(d->fd, VIDIOC_G_SELECTION, &crop)) return -1;
    d->format = raw.fmt.pix_mp;
    if (d->format.num_planes != 1 || crop.r.left || crop.r.top ||
        crop.r.width != (unsigned)w || crop.r.height != (unsigned)h ||
        d->format.width < (unsigned)w || d->format.height < (unsigned)h ||
        d->format.plane_fmt[0].bytesperline < (unsigned)w ||
        (size_t)d->format.plane_fmt[0].bytesperline * d->format.height * 3 / 2 >
            d->format.plane_fmt[0].sizeimage ||
        d->format.pixelformat != V4L2_PIX_FMT_NV12) {
        errno = EINVAL;
        return -1;
    }
    if (allocate_capture(d)) return -1;
    for (int i = 0; i < CAPTURE_BUFFERS; ++i)
        if (queue_buffer(d, raw.type, (unsigned)i, 0)) return -1;
    if (xioctl(d->fd, VIDIOC_STREAMON, &raw.type)) return -1;
    d->capture_on = 1;
    d->width = w; d->height = h;
    return 0;
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
        if (xioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation)) { failed = 1; break; }
        void *va = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, (int)allocation.fd, 0);
        if (va == MAP_FAILED) { close((int)allocation.fd); failed = 1; break; }
        d->nv12[i] = (nv12_slot_t){.fd = (int)allocation.fd, .va = va, .len = len};
    }
    int saved = errno;
    close(heap);
    errno = saved;
    if (failed) { nv12_ring_free(d); return -1; }
    d->nv12_w = w; d->nv12_h = h;
    return 0;
}

static int receive_nv12(jpeg_decoder_t *d, unsigned char *dst)
{
    long long deadline = now_ms() + DECODE_TIMEOUT_MS;
    int output_done = 0, capture_done = 0, failed = 0;
    while (!output_done || !capture_done) {
        for (int capture = 0; capture < 2; ++capture) {
            if (capture ? capture_done : output_done) continue;
            struct v4l2_plane plane = {0};
            struct v4l2_buffer buffer = {.type = capture ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :
                V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                .memory = capture ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP,
                .length = 1, .m.planes = &plane};
            if (xioctl(d->fd, VIDIOC_DQBUF, &buffer)) {
                if (errno == EAGAIN) continue;
                return -1;
            }
            if (buffer.flags & V4L2_BUF_FLAG_ERROR) failed = 1;
            if (!capture) { output_done = 1; continue; }
            if (buffer.index >= CAPTURE_BUFFERS || (buffer.flags & V4L2_BUF_FLAG_LAST) ||
                plane.data_offset || plane.bytesused < d->format.plane_fmt[0].sizeimage ||
                plane.bytesused > d->raw[buffer.index].length) { errno = EIO; return -1; }
            if (!failed) {
                struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
                if (xioctl(d->raw[buffer.index].fd, DMA_BUF_IOCTL_SYNC, &sync)) return -1;
                const unsigned char *base = d->raw[buffer.index].data;
                int stride = (int)d->format.plane_fmt[0].bytesperline;
                size_t w = (size_t)d->width;
                for (int y = 0; y < d->height; ++y)
                    memcpy(dst + (size_t)y * w, base + (size_t)y * stride, w);
                unsigned char *duv = dst + w * (size_t)d->height;
                const unsigned char *suv = base + (size_t)stride * d->format.height;
                for (int y = 0; y < d->height / 2; ++y)
                    memcpy(duv + (size_t)y * w, suv + (size_t)y * stride, w);
                sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                if (xioctl(d->raw[buffer.index].fd, DMA_BUF_IOCTL_SYNC, &sync)) return -1;
            }
            if (queue_buffer(d, buffer.type, buffer.index, 0)) return -1;
            capture_done = 1;
        }
        if (capture_done && output_done) break;
        int remaining = (int)(deadline - now_ms());
        if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd wait = {.fd = d->fd, .events = (short)((capture_done ? 0 : POLLIN) |
                                                             (output_done ? 0 : POLLOUT))};
        int rc = poll(&wait, 1, remaining);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0 || (wait.revents & (POLLNVAL | POLLHUP))) { errno = ETIMEDOUT; return -1; }
    }
    if (failed) { errno = EIO; return -1; }
    return 0;
}

/* 软件解码：jpeg_read_raw_data 取原生分量，不做色度上采样。
   相机 MJPEG 是 4:2:2（h2v1），也有 4:2:0 的型号，两条都支持：
   色度水平 2 合 1，垂直方向 4:2:2 需再 2 行平均。输出 NV12，
   Y 步长 = w，UV 交错步长 = w，与硬件通路的字节布局一致。 */
static int sw_decode_nv12(jpeg_decoder_t *d, const unsigned char *jpeg, size_t length,
                          int w, int h, unsigned char *dst)
{
    if (w % 16 || h % 16) { errno = EINVAL; return -1; }
    if (setjmp(d->sw_err.jump)) { jpeg_abort_decompress(&d->cinfo); errno = EIO; return -1; }
    struct jpeg_decompress_struct *cinfo = &d->cinfo;
    jpeg_mem_src(cinfo, jpeg, (unsigned long)length);
    if (jpeg_read_header(cinfo, TRUE) != JPEG_HEADER_OK) { jpeg_abort_decompress(cinfo); errno = EIO; return -1; }
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
        for (int i = 0; i < crow; i += 2) {   /* 每两条亮度行合成一行 UV */
            unsigned char *line = plane_uv + (size_t)(y / 2 + i / 2) * w;
            const unsigned char *s0b = cbrows[i], *s0r = crrows[i];
            const unsigned char *s1b = vs == 1 ? cbrows[i + 1] : cbrows[i];
            const unsigned char *s1r = vs == 1 ? crrows[i + 1] : crrows[i];
            if (vs == 1) {                    /* 4:2:2：色度已是半宽，只需垂直 2 行平均 */
                for (int x = 0; x < cw; ++x) {
                    line[x * 2] = (unsigned char)((s0b[x] + s1b[x] + 1) >> 1);
                    line[x * 2 + 1] = (unsigned char)((s0r[x] + s1r[x] + 1) >> 1);
                }
            } else {                          /* 4:2:0：色度已是半宽半高，直接使用 */
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

int jpeg_decoder_nv12(jpeg_decoder_t *d, const unsigned char *jpeg, size_t length,
                      int w, int h, int *nv12_fd, unsigned char *copy_to)
{
    if (!d || !jpeg || !nv12_fd || length < 4 || w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
        jpeg[0] != 0xff || jpeg[1] != 0xd8) return -1;
    if (d->sw_ready) {
        int rc = nv12_ring_reserve(d, w, h);
        nv12_slot_t *slot = NULL;
        if (!rc) {
            slot = &d->nv12[d->nv12_next];
            d->nv12_next = (d->nv12_next + 1) % NV12_RING;
            rc = sw_decode_nv12(d, jpeg, length, w, h, slot->va);
        }
        if (!rc && getenv("SAME_FRAME_DUMP")) {
            static int n;
            if (n < 1) {
                n++;
                FILE *jf = fopen("/tmp/same.jpg", "wb");
                if (jf) { fwrite(jpeg, 1, length, jf); fclose(jf); }
                FILE *nf = fopen("/tmp/same.nv12", "wb");
                if (nf) { fwrite(slot->va, 1, (size_t)w * h * 3 / 2, nf); fclose(nf); }
            }
        }
        if (!rc && copy_to) memcpy(copy_to, slot->va, (size_t)w * h * 3 / 2);
        if (!rc) { *nv12_fd = slot->fd; d->width = w; d->height = h; }
        if (rc) {
            int saved = errno;
            unsigned long long errors = atomic_fetch_add(&decode_errors, 1) + 1;
            if (errors <= 3 || errors % 100 == 0)
                LOG_ERROR("jpeg: software decode failed: %s", strerror(saved));
            errno = saved;
        } else atomic_fetch_add(&decoded_frames, 1);
        return rc;
    }
    if (d->width && (d->width != w || d->height != h)) release_buffers(d);
    /* prepare() 内部已排队首帧；后续帧只需重新提交 JPEG，不能重复排队 */
    int fresh = (d->width == 0);
    int rc = fresh ? prepare(d, jpeg, length, w, h) : queue_jpeg(d, jpeg, length);
    if (!rc) rc = nv12_ring_reserve(d, w, h);
    if (!rc) {
        nv12_slot_t *slot = &d->nv12[d->nv12_next];
        d->nv12_next = (d->nv12_next + 1) % NV12_RING;
        rc = receive_nv12(d, slot->va);
        if (!rc && getenv("SAME_FRAME_DUMP")) {
            static int n;
            if (n < 1) {
                n++;
                FILE *jf = fopen("/tmp/same.jpg", "wb");
                if (jf) { fwrite(jpeg, 1, length, jf); fclose(jf); }
                FILE *nf = fopen("/tmp/same.nv12", "wb");
                if (nf) { fwrite(slot->va, 1, (size_t)w * h * 3 / 2, nf); fclose(nf); }
            }
        }
        /* 出队这一刻内核刚把数据交给 CPU；此刻拷贝才不会读到半旧的行 */
        if (!rc && copy_to) memcpy(copy_to, slot->va, (size_t)w * h * 3 / 2);
        if (!rc) *nv12_fd = slot->fd;
    }
    if (rc) {
        int saved = errno;
        unsigned long long errors = atomic_fetch_add(&decode_errors, 1) + 1;
        if (errors <= 3 || errors % 100 == 0)
            LOG_ERROR("jpeg: hardware decode failed on %s: %s", d->device, strerror(saved));
        release_buffers(d);
        errno = saved;
    } else atomic_fetch_add(&decoded_frames, 1);
    return rc;
}

