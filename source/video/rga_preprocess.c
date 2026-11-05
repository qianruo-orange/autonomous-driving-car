#define _GNU_SOURCE
#include "video/rga_preprocess.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-heap.h>

#include "rga/im2d.h"
#include "core/log.h"

#define RGA_CANVAS_HEAP "/dev/dma_heap/default_cma_region"
#define RGA_FILL_VALUE  114

struct rga_preprocess {
    int dst_fd;
    unsigned char *dst_va;
    size_t dst_len;
    int dst_w, dst_h;
    rga_buffer_t dst;
    int ready;
};

static int canvas_alloc(struct rga_preprocess *p, size_t len)
{
    int heap = open(RGA_CANVAS_HEAP, O_RDWR | O_CLOEXEC);
    if (heap < 0) {
        LOG_ERROR("rga: 打开 %s 失败 (%s)；CMA 堆不可用时无法使用 RGA", RGA_CANVAS_HEAP, strerror(errno));
        return -1;
    }
    struct dma_heap_allocation_data alloc = { .len = len, .fd_flags = O_RDWR | O_CLOEXEC };
    int rc = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc);
    int saved = errno;
    close(heap);
    if (rc < 0) {
        LOG_ERROR("rga: CMA 分配 %zu 字节失败 (%s)", len, strerror(saved));
        errno = saved;
        return -1;
    }
    void *va = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, (int)alloc.fd, 0);
    if (va == MAP_FAILED) {
        saved = errno;
        close((int)alloc.fd);
        LOG_ERROR("rga: 画布 mmap 失败 (%s)", strerror(saved));
        errno = saved;
        return -1;
    }
    p->dst_fd = (int)alloc.fd;
    p->dst_va = (unsigned char *)va;
    p->dst_len = len;
    return 0;
}

int rga_preprocess_available(void)
{
    /* 能打开设备即认为可用；画布分配在 open() 里失败时会回退 CPU 路径 */
    int fd = open("/dev/rga", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

rga_preprocess_t *rga_preprocess_open(int dst_w, int dst_h)
{
    if (dst_w <= 0 || dst_h <= 0) return NULL;
    struct rga_preprocess *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->dst_w = dst_w;
    p->dst_h = dst_h;
    size_t len = (size_t)dst_w * (size_t)dst_h * 3u;
    if (canvas_alloc(p, len) != 0) { free(p); return NULL; }

    /* 边框只填一次，之后每帧 RGA 只写中心区域 */
    memset(p->dst_va, RGA_FILL_VALUE, len);
    p->dst = wrapbuffer_fd_t(p->dst_fd, dst_w, dst_h, dst_w, dst_h, RK_FORMAT_RGB_888);
    p->ready = 1;
    LOG_INFO("rga: letterbox 画布 %dx%d RGB888（CMA dma-buf，%.1f MB）", dst_w, dst_h, len / 1048576.0);
    return p;
}

void rga_preprocess_close(rga_preprocess_t *p)
{
    if (!p) return;
    if (p->dst_va && p->dst_va != MAP_FAILED) munmap(p->dst_va, p->dst_len);
    if (p->dst_fd >= 0) close(p->dst_fd);
    free(p);
}

unsigned char *rga_preprocess_run(rga_preprocess_t *p, int src_fd, int src_w, int src_h,
                                  float *scale_x, float *scale_y, int *pad_x, int *pad_y)
{
    if (!p || !p->ready || src_fd < 0 || src_w <= 0 || src_h <= 0) return NULL;

    /* letterbox：等比缩放后居中，与 yolo_rgb_letterbox 的几何一致 */
    float fx = (float)p->dst_w / (float)src_w;
    float fy = (float)p->dst_h / (float)src_h;
    float scale = fx < fy ? fx : fy;
    int rw = (int)((float)src_w * scale + 0.5f);
    int rh = (int)((float)src_h * scale + 0.5f);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    if (rw > p->dst_w) rw = p->dst_w;
    if (rh > p->dst_h) rh = p->dst_h;
    int px = (p->dst_w - rw) / 2;
    int py = (p->dst_h - rh) / 2;

    rga_buffer_t src = wrapbuffer_fd_t(src_fd, src_w, src_h, src_w, src_h, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t pat = {0};
    im_rect srect = { 0, 0, src_w, src_h };
    im_rect drect = { px, py, rw, rh };
    im_rect prect = { 0, 0, 0, 0 };

    IM_STATUS st = improcess(src, p->dst, pat, srect, drect, prect, IM_SYNC);
    if (st != IM_STATUS_SUCCESS) {
        static unsigned long long errors;
        if (!(errors++ % 100u))
            LOG_ERROR("rga: improcess 失败: %s（累计 %llu 次）", imStrError(st), errors);
        return NULL;
    }

    if (scale_x) *scale_x = (float)rw / (float)src_w;
    if (scale_y) *scale_y = (float)rh / (float)src_h;
    if (pad_x) *pad_x = px;
    if (pad_y) *pad_y = py;
    return p->dst_va;
}

unsigned char *rga_preprocess_canvas(const rga_preprocess_t *p) { return p ? p->dst_va : NULL; }
int rga_preprocess_width(const rga_preprocess_t *p) { return p ? p->dst_w : 0; }
int rga_preprocess_height(const rga_preprocess_t *p) { return p ? p->dst_h : 0; }
