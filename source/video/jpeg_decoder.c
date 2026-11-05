/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Stateful V4L2 JPEG decode. The calling image thread owns both queues. */
#define _GNU_SOURCE
#include "video/jpeg_decoder.h"
#include "core/log.h"
#include "yolo/yolo_image.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
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
struct jpeg_decoder {
    int fd, width, height, output_on, capture_on;
    char device[32];
    mapped_buffer_t coded, raw[CAPTURE_BUFFERS];
    struct v4l2_pix_format_mplane format;
    nv12_slot_t nv12[NV12_RING];
    int nv12_next, nv12_w, nv12_h;
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
    d->fd = open_device(d->device, sizeof(d->device));
    if (d->fd < 0) { free(d); return NULL; }
    LOG_INFO("jpeg: hardware decoder %s (rockchip-jpegd)", d->device);
    return d;
}
void jpeg_decoder_close(jpeg_decoder_t *d)
{
    if (!d) return;
    nv12_ring_free(d);
    release_buffers(d);
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

static int receive(jpeg_decoder_t *d, unsigned char *rgb)
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
                /* Cached DMA-BUF avoids repeated CPU reads from device MMAP memory. */
                struct dma_buf_sync sync = {.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
                if (xioctl(d->raw[buffer.index].fd, DMA_BUF_IOCTL_SYNC, &sync)) return -1;
                const unsigned char *base = d->raw[buffer.index].data;
                int stride = (int)d->format.plane_fmt[0].bytesperline;
                yolo_nv12_to_rgb_full(base, base + (size_t)stride * d->format.height,
                                       stride, d->width, d->height, rgb);
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
/* 与 receive() 同构，但把 NV12 直接拷进环形 dma-buf，不做 RGB 转换。
   RGA 会在这块缓冲上一次性完成 CSC + letterbox 缩放。 */
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

int jpeg_decoder_nv12(jpeg_decoder_t *d, const unsigned char *jpeg, size_t length,
                      int w, int h, int *nv12_fd)
{
    if (!d || !jpeg || !nv12_fd || length < 4 || w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
        jpeg[0] != 0xff || jpeg[1] != 0xd8) return -1;
    if (d->width && (d->width != w || d->height != h)) release_buffers(d);
    /* prepare() 内部已排队首帧；后续帧只需重新提交 JPEG，不能重复排队 */
    int fresh = (d->width == 0);
    int rc = fresh ? prepare(d, jpeg, length, w, h) : queue_jpeg(d, jpeg, length);
    if (!rc) rc = nv12_ring_reserve(d, w, h);
    if (!rc) {
        nv12_slot_t *slot = &d->nv12[d->nv12_next];
        d->nv12_next = (d->nv12_next + 1) % NV12_RING;
        rc = receive_nv12(d, slot->va);
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

int jpeg_decoder_rgb(jpeg_decoder_t *d, const unsigned char *jpeg, size_t length,
                     unsigned char *rgb, int w, int h)
{
    if (!d || !jpeg || !rgb || length < 4 || w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
        jpeg[0] != 0xff || jpeg[1] != 0xd8) return -1;
    if (d->width && (d->width != w || d->height != h)) release_buffers(d);
    int rc = d->width ? queue_jpeg(d, jpeg, length) : prepare(d, jpeg, length, w, h);
    if (!rc) rc = receive(d, rgb);
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
