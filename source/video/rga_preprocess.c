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

#include <dlfcn.h>
#include <pthread.h>
#include <time.h>

#include "rga/im2d.h"
#include "core/log.h"

#define RGA_CANVAS_HEAP "/dev/dma_heap/default_cma_region"
#define RGA_FILL_VALUE  114

/* librga 用 dlopen 动态加载（不在 DT_NEEDED 里），这样缺库时进程能启动，
   由上层判定为致命错误并明确报错，而不是死在动态链接阶段。 */
static void *g_lib;
static IM_STATUS (*p_improcess)(rga_buffer_t, rga_buffer_t, rga_buffer_t,
                                im_rect, im_rect, im_rect, int);
static IM_STATUS (*p_improcessOpt)(rga_buffer_t, rga_buffer_t, rga_buffer_t,
                                   im_rect, im_rect, im_rect,
                                   int, int *, im_opt_t *, int);
static rga_buffer_t (*p_wrapbuffer_fd_t)(int, int, int, int, int, int);
static const char *(*p_imStrError)(IM_STATUS);

static int rga_symbols_load(void)
{
    if (g_lib) return 1;
    g_lib = dlopen("librga.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) {
        static int warned;
        if (!warned) { warned = 1; LOG_ERROR("rga: 无法加载 librga.so（%s）", dlerror()); }
        return 0;
    }
    p_improcess = (IM_STATUS (*)(rga_buffer_t, rga_buffer_t, rga_buffer_t,
                                 im_rect, im_rect, im_rect, int))dlsym(g_lib, "improcess");
    p_wrapbuffer_fd_t = (rga_buffer_t (*)(int, int, int, int, int, int))dlsym(g_lib, "wrapbuffer_fd_t");
    /* 错误字符串函数在不同版本导出名不同（imStrError / imStrError_t），可缺省 */
    p_improcessOpt = (IM_STATUS (*)(rga_buffer_t, rga_buffer_t, rga_buffer_t,
                                    im_rect, im_rect, im_rect,
                                    int, int *, im_opt_t *, int))
                     dlsym(g_lib, "improcessOpt");
    p_imStrError = (const char *(*)(IM_STATUS))dlsym(g_lib, "imStrError_t");
    if (!p_imStrError) p_imStrError = (const char *(*)(IM_STATUS))dlsym(g_lib, "imStrError");
    if (!p_improcess || !p_wrapbuffer_fd_t) {
        LOG_ERROR("rga: librga.so 缺少必要符号");
        dlclose(g_lib); g_lib = NULL;
        return 0;
    }
    return 1;
}

/* ---- 按核负载统计（1 秒窗口）---- */
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long load_window_us;
static unsigned long long load_busy_us[2];
static int load_pct[2] = { -1, -1 };
static unsigned long long load_jobs_total;
static int load_active;

static unsigned long long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
}

static void load_account(int core, unsigned long long duration_us)
{
    int idx = (core == RGA_CORE_1) ? 1 : 0;
    unsigned long long now = now_us();
    pthread_mutex_lock(&load_lock);
    if (!load_window_us) load_window_us = now;
    load_busy_us[idx] += duration_us;
    load_jobs_total++;
    unsigned long long elapsed = now - load_window_us;
    if (elapsed >= 1000000ull) {
        for (int i = 0; i < 2; i++) {
            unsigned long long pct = load_busy_us[i] * 100ull / elapsed;
            load_pct[i] = pct > 100ull ? 100 : (int)pct;
            load_busy_us[i] = 0;
        }
        load_window_us = now;
    }
    pthread_mutex_unlock(&load_lock);
}

void rga_preprocess_load(int pct[2], unsigned long long *jobs)
{
    pthread_mutex_lock(&load_lock);
    if (!load_active) {
        pct[0] = pct[1] = -1;
    } else {
        pct[0] = load_pct[0] < 0 ? 0 : load_pct[0];
        pct[1] = load_pct[1] < 0 ? 0 : load_pct[1];
    }
    if (jobs) *jobs = load_jobs_total;
    pthread_mutex_unlock(&load_lock);
}

int rga_preprocess_active(void)
{
    pthread_mutex_lock(&load_lock);
    int active = load_active;
    pthread_mutex_unlock(&load_lock);
    return active;
}

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
    if (!rga_symbols_load()) return 0;
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
    p->dst = p_wrapbuffer_fd_t(p->dst_fd, dst_w, dst_h, dst_w, dst_h, RK_FORMAT_RGB_888);
    p->ready = 1;
    pthread_mutex_lock(&load_lock);
    load_active = 1;
    load_window_us = 0;
    load_busy_us[0] = load_busy_us[1] = 0;
    load_pct[0] = load_pct[1] = 0;
    pthread_mutex_unlock(&load_lock);
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

unsigned char *rga_preprocess_run(rga_preprocess_t *p, int src_fd, int src_w, int src_h, int core,
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

    rga_buffer_t src = p_wrapbuffer_fd_t(src_fd, src_w, src_h, src_w, src_h, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t pat = {0};
    im_rect srect = { 0, 0, src_w, src_h };
    im_rect drect = { px, py, rw, rh };
    im_rect prect = { 0, 0, 0, 0 };

    unsigned long long rga_t0 = now_us();
    /* 按核提交：双核各绑一个推理线程，避免全部落到 core0（librga 默认行为） */
    IM_STATUS st;
    if (core != RGA_CORE_AUTO && p_improcessOpt) {
        im_opt_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.version = RGA_CURRENT_API_HEADER_VERSION;
        /* RK3576 的 RGA2E 属于 RGA2 系列：核选择位是 0x4/0x8（RGA3 的 0x1/0x2 会被驱动判为
           "job assign failed"，实测确认） */
        opt.core = (core == RGA_CORE_1) ? IM_SCHEDULER_RGA2_CORE1 : IM_SCHEDULER_RGA2_CORE0;
        st = p_improcessOpt(src, p->dst, pat, srect, drect, prect, -1, NULL, &opt, IM_SYNC);
    } else {
        st = p_improcess(src, p->dst, pat, srect, drect, prect, IM_SYNC);
    }
    if (st != IM_STATUS_SUCCESS) {
        static unsigned long long errors;
        if (!(errors++ % 100u))
            LOG_ERROR("rga: improcess 失败: %s（累计 %llu 次）",
                      p_imStrError ? p_imStrError(st) : "unknown", errors);
        return NULL;
    }

    load_account(core, now_us() - rga_t0);

    if (scale_x) *scale_x = (float)rw / (float)src_w;
    if (scale_y) *scale_y = (float)rh / (float)src_h;
    if (pad_x) *pad_x = px;
    if (pad_y) *pad_y = py;
    return p->dst_va;
}

unsigned char *rga_preprocess_canvas(const rga_preprocess_t *p) { return p ? p->dst_va : NULL; }
int rga_preprocess_width(const rga_preprocess_t *p) { return p ? p->dst_w : 0; }
int rga_preprocess_height(const rga_preprocess_t *p) { return p ? p->dst_h : 0; }
