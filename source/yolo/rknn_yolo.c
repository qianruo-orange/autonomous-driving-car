/* SPDX-License-Identifier: GPL-3.0-or-later */
/**



 * rknn_yolo.c — RKNN + YOLO26 检测主模块：模型管理 + 多线程推理池 + 按序渲染。
 *
 * 数据流（yolo_task 采集线程 + 自适应推理 worker + 1 渲染 composer）：
 *   yolo_task:  事件驱动等新帧（环条件变量，2s 超时仅用于退出检查）→ 按序
 *             claim 槽 raw（零拷贝引用 V4L2 mmap）→ 锁外解码 RGB24 进池缓冲 →
 *             投递任务队列。claim 随任务移交 worker：任务在途期间槽位被
 *             claims 钉住，不会被采集复用/退让清空
 *   worker i: 出队 → 双线性缩放 → NPU 推理（独立 context，按实际 NPU 核数轮流绑定）→
 *             检测后处理（NMS）→ rgb 写槽 → unclaim；
 *             框、类别和置信度按值入结果队列
 *   composer: 事件驱动消费结果队列，排空只留最新（seq 严格递增，绝不回退；
 *             旧结果条目直接丢弃）→ 运动学跟踪 → 显示 EMA → rgb 原地画框 →
 *             标注帧只用于检测报告，不再重编码（display_done=1，display_seq 推进）；
 *             渲染节拍 ~30fps
 *
 * 帧数据全部走视频帧环形队列（video/frame_ring.h）：采集 raw（V4L2 mmap）、
 * 解码 rgb 等阶段缓冲只在槽位间移动指针，零拷贝。
 * 直播与录像直接取相机原始 JPEG，不经过本模块的标注帧。
 *
 * 渲染 seq 单调不回退，画面不会抖动/闪烁；被跳过的旧结果直接回池。
 *
 * 热重载：rknn_yolo_reload() 停止推理池（pool_running=0 → 线程退出/join，
 * 在途任务 rgb 回池 + 摘除槽 claim）→ 释放模型与 context → 重读配置
 * （yolo_model/yolo_names/yolo_conf/yolo_nms；并发度按帧率和实测耗时自动调整）
 * → 重建池。由前端上传模型/标签文件与 YOLO 参数保存触发。
 *
 * 必要流程：YOLO 推理不可停用。无模型 / NPU 驱动未加载 / 推理失败 → 直接报错：
 * 启动路径整体失败退出；运行期热重载失败由 HTTP 接口报告 saved_reload_failed。
 *
 * 后处理已拆分为独立模块（yolo_postprocess），图像预处理在 RGA 上完成（video/rga_preprocess），
 * 本文件仅保留：模型生命周期、推理线程池、任务队列、结果队列、
 * 环形队列槽消费、快照与对外 API。
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdatomic.h>

#include "yolo/rknn_api.h"
#include "video/video_stream.h"

#include "yolo/rknn_yolo.h"
#include "control/car_sport_ctrl.h"
#include "yolo/yolo_postprocess.h"
#include "yolo/yolo_classes.h"
#include "control/image_tracking.h"
#include "yolo/yolo_events.h"
#include "core/common.h"
#include "core/thread.h"
#include "core/log.h"
#include "core/config.h"
#include "watchdog/watchdog.h"
#include "video/video_stream.h"
#include "video/frame_ring.h"
#include "video/jpeg_decoder.h"
#include "video/rga_preprocess.h"
#include "video/video_rec.h"

/* 标注录像的 NV12 暂存：解码器出队瞬间拷进来，之后读的就是这份，
   不再依赖可能已被复用的解码器缓冲。__thread 保证每 worker 独立。 */
static __thread unsigned char *annot_buf;
static __thread size_t annot_cap;


#define YOLO_MAX_WORKERS 2     /* RK3576：NPU 双核，一核一个线程 */
#define YOLO_QUEUE_CAP   4     /* 任务队列容量（帧），与 worker 数共同约束在途帧内存 */
#define YOLO_MAX_INFLIGHT 2    /* 在途上限（帧）：解码并行后不压深度会让延迟累积到 ~170 ms */
#define YOLO_RESULT_CAP  8     /* 结果队列容量（帧），推理结果入队，composer 消费渲染 */
#define YOLO_HIST_N      16    /* 最近结果环形缓冲深度（供前端按 seq 对齐叠框） */

/* 推理任务：解码后的 RGB 帧（池内缓冲，由消费它的 worker 写槽/回池）。
   槽 claim 随任务移交：yolo_task claim 后不摘除，worker 写槽时一并 unclaim */
typedef struct {
    unsigned char    *rgb;
    size_t            rgb_cap;
    int               nv12_fd;   /* >=0：RGA 路径，解码器 NV12 dma-buf（rgb 为 NULL） */
    unsigned char    *annot_nv12; /* 标注录像：解码时拷好的一整帧 NV12 */
    int               w, h;
    unsigned long long seq;
    unsigned long long capture_ms, deliver_ms, decode_start_us, decode_end_us;
    yolo_event_meta_t metadata;
} yolo_job_t;

static void yolo_event_capture_meta_locked(const frame_slot_t *slot, yolo_event_meta_t *meta);
static int yolo_event_describe_result(yolo_event_meta_t *meta, const yolo_result_t *result,
                                      unsigned long long now_monotonic_ms,
                                      unsigned long long now_unix_ms);
static void yolo_record_result(const yolo_job_t *job, const yolo_result_t *result);

/* 单个推理工作线程：独立 rknn context，可与其它线程并行 run */
typedef struct {
    rknn_context    ctx;
    unsigned char  *in_buf;                 /* 缩放后输入缓冲 */
    rknn_tensor_attr out_attr[2];           /* 单输出解码结果，或 one2one 的类别/框两路 */
    unsigned         out_count;
} yolo_worker_t;

/* 模块全局上下文 */
static struct {
    _Atomic int    enabled;                  /* 推理池可用（画框流可用） */
    _Atomic int    running;                  /* 模块生命周期（destroy 置 0） */
    _Atomic int    pool_running;             /* 推理池生命周期（reload 置 0 停池） */
    _Atomic int    decoder_ready;      /* 解码器是否就绪，仅用于状态上报 */
    app_ctx_t     *app;
    float          conf, nms;
    int            nthreads;                 /* Workers evenly assigned to detected NPU cores. */
    int            active_workers;
    int            events_initialized;

    rga_preprocess_t *rga;                   /* RGA 硬件预处理（NULL = 回退 CPU 路径） */
    unsigned char *model;                    /* 模型文件内存（共享给所有 worker init） */
    size_t         model_len;
    int            in_w, in_h;

    yolo_worker_t  workers[YOLO_MAX_WORKERS];

    /* 有界任务队列（采集线程生产 / worker 消费） */
    yolo_job_t     jobs[YOLO_QUEUE_CAP];
    int            q_head, q_tail, q_count;
    pthread_mutex_t q_lock;
    pthread_cond_t  q_not_empty;
    pthread_cond_t  q_not_full;

    /* 有界结果队列（worker 生产推理结果 / composer 消费渲染） */
    yolo_result_t results[YOLO_RESULT_CAP];
    int            r_head, r_tail, r_count;
    pthread_mutex_t r_lock;
    pthread_cond_t  r_not_empty;
    pthread_cond_t  r_not_full;

    pthread_t       w_tid[YOLO_MAX_WORKERS];
    pthread_t       render_tid;
    int             n_created;               /* 实际创建成功的 worker 线程数（停池据此 join） */

    unsigned long long next_seq;             /* 渲染游标：下一个待渲染 seq（环 seq 单调，0 起步，
                                               跨 reload 保留不重复渲染） */

    /* EMA 平滑状态（仅 composer 线程访问） */
    yolo_result_t   prev;

    /* 类别名表（yolo_names 文件，缺失回退内置 COCO 80） */
    yolo_classes_t  classes;
    char            names_path[256];

    unsigned long long last_seq;             /* 采集游标：最近已解码/消费的帧 seq */

    /* 快照：composer 按序写入，仅当新 seq 大于当前时更新 */
    pthread_mutex_t result_mutex;
    yolo_result_t   result;

    /* 最近结果环形缓冲：供 HTTP 增量拉取，前端按 seq 把框对齐到 MJPEG 原始帧。
       YOLO_HIST_N 帧 @30fps ≈ 530ms 历史，足够覆盖推流与轮询的抖动差。 */
    yolo_result_t   hist[YOLO_HIST_N];
    int             hist_count;
    int             hist_next;               /* 下一次写入位置（环形） */
} g_yolo;

static pthread_mutex_t g_reload_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_producer_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_reloading;

static unsigned long long yolo_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000u + (unsigned long long)ts.tv_nsec / 1000u;
}

int rknn_yolo_active_workers(void)
{
    return g_yolo.enabled ? __atomic_load_n(&g_yolo.active_workers, __ATOMIC_RELAXED) : 0;
}

/* ---- 环形队列访问 ---- */

/* 环句柄：video 模块先于 yolo 初始化，正常非 NULL；防御性判空由调用方处理 */
static frame_ring_t *yolo_ring(void)
{
    return video_stream_get_ring();
}

static frame_slot_t *yolo_slot(frame_ring_t *r, unsigned long long seq)
{
    return &r->slots[seq & (FRAME_RING_N - 1)];
}

/* 未完成的 claim 数（在途帧）。解码并行后生产追平消费，队列会永久积压，
   用它把流水线深度压住。调用方需持环锁。 */
static int yolo_inflight_locked(frame_ring_t *r)
{
    int n = 0;
    for (int i = 0; i < FRAME_RING_N; i++)
        if (r->slots[i].raw_claims > 0) n++;
    return n;
}

/* 任务作废（停池/推理失败/队列拒绝）：摘除槽 claim（raw 恢复可释放）
   并把 rgb 归还池。解码已成功，作废不计 infer_dropped */
static void yolo_job_abandon(const yolo_job_t *job)
{
    frame_ring_t *ring = yolo_ring();
    if (!ring) return;
    frame_ring_lock(ring);
    frame_slot_t *s = yolo_slot(ring, job->seq);
    if (s->seq == job->seq && s->raw_claims > 0)
        frame_ring_infer_unclaim_locked(ring, s, 1);
    if (job->rgb)
        frame_ring_buf_put_locked(ring, FRAME_RING_POOL_RGB, job->rgb, job->rgb_cap);
    frame_ring_unlock(ring);
}

/* 解码成功但任务没进推理队列（停池/退出）：abandon 只管 claim 与 rgb，
   解码线程 malloc 的 NV12 副本要在这里一起释放。 */
static void yolo_job_discard(yolo_job_t *job)
{
    yolo_job_abandon(job);
    free(job->annot_nv12);
    job->annot_nv12 = NULL;
}

/* ---- 解码阶段：两个独立 libjpeg 实例并行 ----
 *
 * 采集线程只做 claim 和投递，解码挪到独立线程。顺序仍是 seq 递增：队列 FIFO、
 * 每个线程独立解码，完成后直接进推理队列（推理侧本来就按完成时间消费）。
 *
 * 队列深度等于线程数，投递前先等空位（背压），所以投递本身不会阻塞，可以
 * 在整个 claim 窗口里持 g_producer_mutex，与停池清理共用同一把锁。 */
#define YOLO_DECODE_THREADS 2

static int yolo_queue_push(const yolo_job_t *job);

typedef struct {
    yolo_job_t           job;       /* 元数据已填，nv12_fd/annot_nv12 由解码线程补 */
    const unsigned char *raw;
    size_t               raw_len;
} decode_item_t;

static struct {
    decode_item_t   q[YOLO_DECODE_THREADS];
    int             head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    pthread_t       tid[YOLO_DECODE_THREADS];
    int             created;
    _Atomic int     stop;
} g_decode;

static void *yolo_decode_task(void *arg)
{
    (void)arg;
    (void)pthread_setname_np(pthread_self(), "decode");
    /* 每个线程一个 decoder：jpeg_decompress_struct 不可跨线程共用 */
    jpeg_decoder_t *decoder = jpeg_decoder_open();
    if (!decoder) {
        /* 与旧行为一致：没有解码器就等于没有 YOLO，整体退出让 systemd 重启 */
        LOG_ERROR("decode: libjpeg decoder open failed — stopping");
        atomic_store(&g_yolo.decoder_ready, 0);
        g_yolo.app->fatal = 1;
        g_yolo.app->running = 0;
    } else {
        atomic_store(&g_yolo.decoder_ready, 1);
    }

    for (;;) {
        pthread_mutex_lock(&g_decode.lock);
        while (!g_decode.count && !atomic_load(&g_decode.stop) && g_yolo.pool_running)
            pthread_cond_wait(&g_decode.not_empty, &g_decode.lock);
        if (atomic_load(&g_decode.stop) || !g_yolo.pool_running || !g_decode.count) {
            pthread_mutex_unlock(&g_decode.lock);
            break;
        }
        decode_item_t item = g_decode.q[g_decode.head];
        g_decode.head = (g_decode.head + 1) % YOLO_DECODE_THREADS;
        g_decode.count--;
        pthread_cond_signal(&g_decode.not_full);
        pthread_mutex_unlock(&g_decode.lock);

        yolo_job_t job = item.job;
        unsigned char *annot = NULL;
        int fd = -1, ok = 0;
        if (decoder) {
            size_t need = (size_t)job.w * (size_t)job.h * 3 / 2;
            annot = malloc(need);
            ok = annot && jpeg_decoder_nv12(decoder, item.raw, item.raw_len,
                                            job.w, job.h, &fd, annot) == 0;
        }
        job.decode_end_us = yolo_now_us();
        if (!ok) {
            free(annot);
            /* 解码失败：摘 claim 并计 infer_dropped（与旧内联路径一致） */
            frame_ring_t *ring = yolo_ring();
            if (ring) {
                frame_ring_lock(ring);
                frame_slot_t *s = yolo_slot(ring, job.seq);
                if (s->seq == job.seq && s->raw_claims > 0)
                    frame_ring_infer_unclaim_locked(ring, s, 0);
                frame_ring_unlock(ring);
            }
            continue;
        }
        job.nv12_fd = fd;
        job.annot_nv12 = annot;

        /* 与停池互斥：push 前持生产者锁，停池清理队列时不会漏掉这一帧 */
        pthread_mutex_lock(&g_producer_mutex);
        if (!g_yolo.running || !g_yolo.pool_running || yolo_queue_push(&job) != 0)
            yolo_job_discard(&job);
        pthread_mutex_unlock(&g_producer_mutex);
    }
    jpeg_decoder_close(decoder);
    return NULL;
}

/* 投递前等空位：满队列时在这里背压，不把 claim 的槽位堆在解码队列里 */
static int yolo_decode_wait_room(void)
{
    pthread_mutex_lock(&g_decode.lock);
    while (g_decode.count >= YOLO_DECODE_THREADS && !atomic_load(&g_decode.stop) &&
           g_yolo.running && g_yolo.pool_running)
        pthread_cond_wait(&g_decode.not_full, &g_decode.lock);
    int ready = !atomic_load(&g_decode.stop) && g_yolo.running && g_yolo.pool_running;
    pthread_mutex_unlock(&g_decode.lock);
    return ready ? 0 : -1;
}

/* 调用方已确认有空位（只有采集线程一个生产者） */
static void yolo_decode_submit(const decode_item_t *item)
{
    pthread_mutex_lock(&g_decode.lock);
    g_decode.q[g_decode.tail] = *item;
    g_decode.tail = (g_decode.tail + 1) % YOLO_DECODE_THREADS;
    g_decode.count++;
    pthread_cond_signal(&g_decode.not_empty);
    pthread_mutex_unlock(&g_decode.lock);
}

static void yolo_decode_stop(void);

static int yolo_decode_start(void)
{
    atomic_store(&g_decode.stop, 0);
    g_decode.head = g_decode.tail = g_decode.count = 0;
    for (int i = 0; i < YOLO_DECODE_THREADS; i++) {
        if (app_thread_create(&g_decode.tid[i], "decode", yolo_decode_task, NULL, 0) != 0) {
            LOG_ERROR("decode: thread %d create failed", i);
            yolo_decode_stop();
            return -1;
        }
        g_decode.created = i + 1;
    }
    LOG_INFO("decode: %d libjpeg worker threads on the decode pair", YOLO_DECODE_THREADS);
    return 0;
}

static void yolo_decode_stop(void)
{
    pthread_mutex_lock(&g_decode.lock);
    atomic_store(&g_decode.stop, 1);
    pthread_cond_broadcast(&g_decode.not_empty);
    pthread_cond_broadcast(&g_decode.not_full);
    pthread_mutex_unlock(&g_decode.lock);
    for (int i = 0; i < g_decode.created; i++) pthread_join(g_decode.tid[i], NULL);
    g_decode.created = 0;
    /* 队列里没被取走的任务：摘 claim 并释放解码副本 */
    for (int i = 0; i < g_decode.count; i++)
        yolo_job_discard(&g_decode.q[(g_decode.head + i) % YOLO_DECODE_THREADS].job);
    g_decode.head = g_decode.tail = g_decode.count = 0;
}

/* ---- 任务队列 ---- */

static int yolo_queue_push(const yolo_job_t *job)
{
    pthread_mutex_lock(&g_yolo.q_lock);
    while (g_yolo.q_count >= YOLO_QUEUE_CAP && g_yolo.running && g_yolo.pool_running)
        pthread_cond_wait(&g_yolo.q_not_full, &g_yolo.q_lock);
    if (!g_yolo.running || !g_yolo.pool_running) {   /* 退出/停池：拒绝投递 */
        pthread_mutex_unlock(&g_yolo.q_lock);
        return -1;
    }
    g_yolo.jobs[g_yolo.q_tail] = *job;
    g_yolo.q_tail = (g_yolo.q_tail + 1) % YOLO_QUEUE_CAP;
    g_yolo.q_count++;
    /* Parked and active workers share this condition. A signal may select a
       parked worker, leaving the queued frame asleep until another arrives. */
    pthread_cond_broadcast(&g_yolo.q_not_empty);
    pthread_mutex_unlock(&g_yolo.q_lock);
    return 0;
}

static int yolo_queue_pop(yolo_job_t *job, int worker_index)
{
    pthread_mutex_lock(&g_yolo.q_lock);
    while ((g_yolo.q_count == 0 || worker_index >= __atomic_load_n(&g_yolo.active_workers, __ATOMIC_RELAXED)) && g_yolo.running && g_yolo.pool_running)
        pthread_cond_wait(&g_yolo.q_not_empty, &g_yolo.q_lock);
    if (g_yolo.q_count == 0 || !g_yolo.running || !g_yolo.pool_running) {
        pthread_mutex_unlock(&g_yolo.q_lock);
        return -1;
    }
    *job = g_yolo.jobs[g_yolo.q_head];
    g_yolo.jobs[g_yolo.q_head].rgb = NULL;   /* 所有权已移交，防止停池清理时 double-free */
    g_yolo.q_head = (g_yolo.q_head + 1) % YOLO_QUEUE_CAP;
    g_yolo.q_count--;
    pthread_cond_signal(&g_yolo.q_not_full);
    pthread_mutex_unlock(&g_yolo.q_lock);
    return 0;
}

/* ---- 推理结果队列（worker 生产 / composer 消费）---- */

static int yolo_result_push(const yolo_result_t *item)
{
    pthread_mutex_lock(&g_yolo.r_lock);
    while (g_yolo.r_count >= YOLO_RESULT_CAP && g_yolo.running && g_yolo.pool_running)
        pthread_cond_wait(&g_yolo.r_not_full, &g_yolo.r_lock);
    if (!g_yolo.running || !g_yolo.pool_running) {   /* 退出/停池：拒绝入队 */
        pthread_mutex_unlock(&g_yolo.r_lock);
        return -1;
    }
    g_yolo.results[g_yolo.r_tail] = *item;
    g_yolo.r_tail = (g_yolo.r_tail + 1) % YOLO_RESULT_CAP;
    g_yolo.r_count++;
    pthread_cond_signal(&g_yolo.r_not_empty);
    pthread_mutex_unlock(&g_yolo.r_lock);
    return 0;
}

/* 非阻塞出队：队列空返回 -1 */
static int yolo_result_try_pop(yolo_result_t *item)
{
    pthread_mutex_lock(&g_yolo.r_lock);
    if (g_yolo.r_count == 0) {
        pthread_mutex_unlock(&g_yolo.r_lock);
        return -1;
    }
    *item = g_yolo.results[g_yolo.r_head];
    g_yolo.r_head = (g_yolo.r_head + 1) % YOLO_RESULT_CAP;
    g_yolo.r_count--;
    pthread_cond_signal(&g_yolo.r_not_full);
    pthread_mutex_unlock(&g_yolo.r_lock);
    return 0;
}

/* 阻塞出队：timeout_ms 超时仅用于调用方周期性检查退出条件；
   超时/停池/退出且队列空返回 -1 */
static int yolo_result_pop(yolo_result_t *item, int timeout_ms)
{
    pthread_mutex_lock(&g_yolo.r_lock);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (g_yolo.r_count == 0 && g_yolo.running && g_yolo.pool_running) {
        if (pthread_cond_timedwait(&g_yolo.r_not_empty, &g_yolo.r_lock, &ts) != 0) {
            pthread_mutex_unlock(&g_yolo.r_lock);
            return -1;
        }
    }
    if (g_yolo.r_count == 0) {
        pthread_mutex_unlock(&g_yolo.r_lock);
        return -1;
    }
    *item = g_yolo.results[g_yolo.r_head];
    g_yolo.r_head = (g_yolo.r_head + 1) % YOLO_RESULT_CAP;
    g_yolo.r_count--;
    pthread_cond_signal(&g_yolo.r_not_full);
    pthread_mutex_unlock(&g_yolo.r_lock);
    return 0;
}

/* Worker completion order is not capture order. Keep the largest sequence,
   rather than the last arrival, when coalescing a burst of ready results. */
static int yolo_result_pop_latest(yolo_result_t *item, int timeout_ms)
{
    if (yolo_result_pop(item, timeout_ms) != 0) return -1;
    yolo_result_t newer;
    while (yolo_result_try_pop(&newer) == 0)
        if (newer.seq > item->seq) *item = newer;
    return 0;
}

/* ---- 结果快照（composer 按序更新，单调递增） ---- */

static void snap_result(const yolo_result_t *res)
{
    pthread_mutex_lock(&g_yolo.result_mutex);
    if (res->seq > g_yolo.result.seq) {
        g_yolo.result = *res;
        /* 环形追加：只有更新的 seq 才入历史，保证 hist 内 seq 单调递增 */
        g_yolo.hist[g_yolo.hist_next] = *res;
        g_yolo.hist_next = (g_yolo.hist_next + 1) % YOLO_HIST_N;
        if (g_yolo.hist_count < YOLO_HIST_N) g_yolo.hist_count++;
    }
    pthread_mutex_unlock(&g_yolo.result_mutex);
}

int rknn_yolo_results_since(unsigned long long after, yolo_result_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = 0;
    pthread_mutex_lock(&g_yolo.result_mutex);
    int count = g_yolo.hist_count;
    /* 最旧一条的位置：环形里有效数据从 (hist_next - count) 开始 */
    int start = (g_yolo.hist_next - count + YOLO_HIST_N * 2) % YOLO_HIST_N;
    for (int i = 0; i < count && n < max; i++) {
        const yolo_result_t *r = &g_yolo.hist[(start + i) % YOLO_HIST_N];
        if (r->seq <= after) continue;
        out[n++] = *r;
    }
    pthread_mutex_unlock(&g_yolo.result_mutex);
    return n;
}

/* 类别名（只读）：供前端叠加标注框时显示与烧框一致的标签；越界返回 "object"。 */
const char *rknn_yolo_class_name(int cls)
{
    if (cls < 0 || cls >= g_yolo.classes.count) return "object";
    return g_yolo.classes.names[cls];
}

/* 时间平滑：cur 中与 prev 同类且 IoU 足够大的框，位置/置信度做 EMA
   （新帧权重 0.4，保留 0.6 历史），降低逐帧检测抖动 */
static void yolo_smooth(const yolo_result_t *prev, yolo_result_t *cur)
{
    if (prev->count <= 0 || cur->count <= 0 || prev->w != cur->w || prev->h != cur->h) return;
    const float alpha = 0.4f;
    for (int i = 0; i < cur->count; i++) {
        yolo_det_t *d = &cur->dets[i];
        float best_iou = 0.3f;
        int bi = -1;
        for (int j = 0; j < prev->count; j++) {
            const yolo_det_t *p = &prev->dets[j];
            if (p->cls != d->cls) continue;
            float ix1 = d->x1 > p->x1 ? d->x1 : p->x1;
            float iy1 = d->y1 > p->y1 ? d->y1 : p->y1;
            float ix2 = d->x2 < p->x2 ? d->x2 : p->x2;
            float iy2 = d->y2 < p->y2 ? d->y2 : p->y2;
            if (ix2 <= ix1 || iy2 <= iy1) continue;
            float inter = (ix2 - ix1) * (iy2 - iy1);
            float uni = (d->x2-d->x1)*(d->y2-d->y1) + (p->x2-p->x1)*(p->y2-p->y1) - inter;
            if (uni <= 0.0f) continue;
            float iou = inter / uni;
            if (iou > best_iou) { best_iou = iou; bi = j; }
        }
        if (bi >= 0) {
            const yolo_det_t *p = &prev->dets[bi];
            d->x1 = alpha*d->x1 + (1.0f-alpha)*p->x1;
            d->y1 = alpha*d->y1 + (1.0f-alpha)*p->y1;
            d->x2 = alpha*d->x2 + (1.0f-alpha)*p->x2;
            d->y2 = alpha*d->y2 + (1.0f-alpha)*p->y2;
            d->conf = alpha*d->conf + (1.0f-alpha)*p->conf;
        }
    }
}

/* ---- 推理工作线程 ---- */

static void *yolo_worker_task(void *arg)
{
    int idx = (int)(intptr_t)arg;
    (void)pthread_setname_np(pthread_self(), "yolo_infer");
    yolo_worker_t *wk = &g_yolo.workers[idx];
    frame_ring_t *ring = yolo_ring();

    while (g_yolo.running && g_yolo.pool_running) {
        if (!ring) { usleep(200000); continue; }   /* 防御：video 未初始化 */

        yolo_job_t job;
        if (yolo_queue_pop(&job, idx) < 0) break;
        if (!g_yolo.running || !g_yolo.pool_running) { yolo_job_abandon(&job); break; }
        unsigned long long worker_us = yolo_now_us();

        /* letterbox 预处理：只走 RGA（NV12 dma-buf 一趟完成 CSC + 缩放）。
           几何参数供后处理逆映射。 */
        float lb_scale_x = 1.0f, lb_scale_y = 1.0f; int lb_pad_x = 0, lb_pad_y = 0;
        unsigned char *model_in = NULL;
        if (job.nv12_fd >= 0 && g_yolo.rga)
            model_in = rga_preprocess_run(g_yolo.rga, job.nv12_fd, job.w, job.h,
                                          (idx & 1) ? RGA_CORE_1 : RGA_CORE_0,
                                          &lb_scale_x, &lb_scale_y, &lb_pad_x, &lb_pad_y);
        if (!model_in) {
            static unsigned long long rga_fail;
            if (!(rga_fail++ % 100u))
                LOG_ERROR("yolo: RGA letterbox 失败，任务作废（累计 %llu）", rga_fail);
            yolo_job_abandon(&job);
            continue;
        }
        unsigned long long prepared_us = yolo_now_us();

        rknn_input in;
        memset(&in, 0, sizeof(in));
        in.index = 0;
        in.type  = RKNN_TENSOR_UINT8;
        in.fmt   = RKNN_TENSOR_NHWC;
        in.size  = (uint32_t)(g_yolo.in_w * g_yolo.in_h * 3);
        in.buf   = model_in;
        if (rknn_inputs_set(wk->ctx, 1, &in) != RKNN_SUCC) { yolo_job_abandon(&job); continue; }
        unsigned long long inputs_us = yolo_now_us();
        if (rknn_run(wk->ctx, NULL) != RKNN_SUCC) { yolo_job_abandon(&job); continue; }
        unsigned long long inference_us = yolo_now_us();

        rknn_output outputs[2];
        memset(outputs, 0, sizeof(outputs));
        const unsigned nout = wk->out_count;
        for (unsigned i = 0; i < nout; i++) {
            outputs[i].index = i;
            /* 单输出沿用 float 解码；双输出按各路的实际 dtype 取数据：
               INT8（类别分）走原生缓冲由后处理反量化，FP16（框）让运行时转 float */
            outputs[i].want_float = (nout == 1) ? 1
                                 : (wk->out_attr[i].type == RKNN_TENSOR_FLOAT16 ? 1 : 0);
        }
        if (rknn_outputs_get(wk->ctx, nout, outputs, NULL) != RKNN_SUCC) {
            yolo_job_abandon(&job);
            continue;
        }
        unsigned long long outputs_us = yolo_now_us();

        yolo_result_t res;
        memset(&res, 0, sizeof(res));
        res.seq = job.seq;
        res.w   = job.w;
        res.h   = job.h;
        if (nout == 2) {
            /* YOLO26 one2one：类别 (1,80,A) + 框 (1,4,A)，无需 NMS */
            const rknn_tensor_attr *sa = &wk->out_attr[0];
            const rknn_tensor_attr *ba = &wk->out_attr[1];
            const void *sbuf = outputs[0].buf, *bbuf = outputs[1].buf;
            if (sa->dims[1] == 4 || sa->dims[2] == 4) {
                sa = &wk->out_attr[1]; ba = &wk->out_attr[0];
                sbuf = outputs[1].buf;  bbuf = outputs[0].buf;
            }
            res.count = yolo_postprocess_two(sbuf, sa, bbuf, ba,
                                             lb_scale_x, lb_scale_y, lb_pad_x, lb_pad_y,
                                             job.w, job.h, res.dets, YOLO_MAX_DETS,
                                             g_yolo.conf, g_yolo.nms, NULL);
        } else {
            const float *out_buf[1] = { (const float *)outputs[0].buf };
            res.count = yolo_postprocess(out_buf, wk->out_attr, 1,
                                         lb_scale_x, lb_scale_y, lb_pad_x, lb_pad_y, job.w, job.h,
                                         res.dets, YOLO_MAX_DETS, g_yolo.conf, g_yolo.nms, NULL);
        }
        rknn_outputs_release(wk->ctx, nout, outputs);
        /* Persist every completed inference before the display queue can
           coalesce it. Empty detections are records too; no photo is encoded. */
        res.decode_end_us = job.decode_end_us;
        res.post_done_us = yolo_now_us();
        yolo_record_result(&job, &res);
        /* 标注录像：把本帧 NV12 与检测框交给编码线程（内部立即拷贝） */
        if (job.annot_nv12) {
            float xyxy[YOLO_MAX_DETS * 4];
            int n = res.count > YOLO_MAX_DETS ? YOLO_MAX_DETS : res.count;
            for (int i = 0; i < n; i++) {
                xyxy[4 * i + 0] = res.dets[i].x1;
                xyxy[4 * i + 1] = res.dets[i].y1;
                xyxy[4 * i + 2] = res.dets[i].x2;
                xyxy[4 * i + 3] = res.dets[i].y2;
            }
            video_rec_annot_frame(job.annot_nv12, job.w, job.h, xyxy, n);
        }
        free(job.annot_nv12);
        job.annot_nv12 = NULL;
        if (idx == 0) {
            static unsigned long long last_timing_us;
            unsigned long long finished_us = yolo_now_us();
            if (finished_us - last_timing_us >= 10000000u) {
                last_timing_us = finished_us;
                LOG_INFO("yolo timing ms: capture_to_decode=%llu deliver_to_decode=%llu decode=%llu queue=%llu resize=%llu inputs=%llu run=%llu outputs=%llu post=%llu",
                         job.decode_start_us / 1000u - job.capture_ms,
                         job.deliver_ms ? job.decode_start_us / 1000u - job.deliver_ms : 0,
                         (job.decode_end_us - job.decode_start_us) / 1000u,
                         (worker_us - job.decode_end_us) / 1000u,
                         (prepared_us - worker_us) / 1000u,
                         (inputs_us - prepared_us) / 1000u,
                         (inference_us - inputs_us) / 1000u,
                         (outputs_us - inference_us) / 1000u,
                         (finished_us - outputs_us) / 1000u);
            }
        }

        /* 停池竞态：推理期间 pool_running 被清 → 作废任务（不写槽，
           避免停池后槽内滞留 rgb 阻碍该槽复用） */
        if (!g_yolo.running || !g_yolo.pool_running) { yolo_job_abandon(&job); continue; }

        /* rgb 写槽（claim 钉住槽位：任务在途期间不会被复用/清空，
           故 s->seq == job.seq 必然成立；防御分支防退让强制清空的迟到写） */
        frame_ring_lock(ring);
        frame_slot_t *s = yolo_slot(ring, job.seq);
        if (s->seq == job.seq && s->raw_claims > 0) {
            s->rgb.buf  = job.rgb;
            s->rgb.cap  = job.rgb_cap;
            s->rgb.len  = job.rgb ? (size_t)job.w * job.h * 3 : 0;
            s->rgb_done = 1;   /* 结果就绪；RGA 路径下 rgb 为空，渲染只用 w/h */
            frame_ring_infer_unclaim_locked(ring, s, 1);
        } else {
            if (job.rgb)
                frame_ring_buf_put_locked(ring, FRAME_RING_POOL_RGB, job.rgb, job.rgb_cap);
            if (s->seq == job.seq && s->raw_claims > 0)
                frame_ring_infer_unclaim_locked(ring, s, 1);
        }
        frame_ring_unlock(ring);

        /* Detection results travel through the queue by value. */
        yolo_result_push(&res);
    }
    return NULL;
}

/* ---- 渲染 composer：结果队列最新结果优先（seq 单调不回退）+ ~30fps 节拍 ----

   严格按 seq 顺序消费会因 worker 乱序完成而等待缺口（实测卡顿 100~700ms）。
   改为消费结果队列中最新完成结果：渲染 seq 严格递增（绝不回退，画面不抖动），
   被跳过的旧结果条目直接丢弃（纯元数据，无资源）；EMA 平滑本身即可容忍帧间小跳跃。
   画框后的 RGB 转换为 NV12，随后由硬件编码器发布视频。 */

/* Move ownership out of the slot before unlocking. Camera quiesce may clear
 * every slot while rendering; it must never recycle an in-flight RGB buffer. */
static int yolo_render_take_rgb_locked(frame_ring_t *ring, unsigned long long seq,
                                      ring_buf_t *rgb, int *w, int *h)
{
    frame_slot_t *slot = yolo_slot(ring, seq);
    if (ring->quiescing || slot->seq != seq || !slot->rgb_done)
        return -1;
    /* RGA 路径不产生 RGB：渲染只需要帧尺寸（检测框由浏览器 canvas 绘制） */
    *w = slot->w;
    *h = slot->h;
    *rgb = slot->rgb;
    memset(&slot->rgb, 0, sizeof(slot->rgb));
    return 0;
}

/* Called while the matching slot is pinned by the renderer's ring lock. Lens
   readings belong to this captured frame, never to a later HTTP status poll. */
static void yolo_event_capture_meta_locked(const frame_slot_t *slot, yolo_event_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));
    meta->seq = slot->seq;
    meta->capture_monotonic_ms = slot->capture_ms;
    meta->width = slot->w;
    meta->height = slot->h;
    meta->focus_valid = !!slot->focus_valid;
    meta->focus_position = meta->focus_valid ? slot->focus_position : 0;
    meta->autofocus = slot->autofocus == 0 || slot->autofocus == 1 ? slot->autofocus : -1;
    meta->focus_sample_monotonic_ms = slot->focus_sample_ms;
    snprintf(meta->camera, sizeof(meta->camera), "%.*s", (int)sizeof(slot->camera) - 1,
             slot->camera[0] ? slot->camera : "Unknown");
}

/* Describe one completed inference using its own captured frame metadata.
   Unix capture time is estimated from the wall/monotonic clock offset. */
static int yolo_event_describe_result(yolo_event_meta_t *meta, const yolo_result_t *display,
                                       unsigned long long now_monotonic_ms,
                                       unsigned long long now_unix_ms)
{
    if (meta->seq != display->seq || meta->width != display->w || meta->height != display->h ||
        !meta->capture_monotonic_ms) return -1;
    unsigned long long age = now_monotonic_ms >= meta->capture_monotonic_ms ?
                             now_monotonic_ms - meta->capture_monotonic_ms : 0;
    meta->capture_unix_ms = now_unix_ms >= age ? now_unix_ms - age : now_unix_ms;
    meta->count = display->count < 0 ? 0 : display->count > YOLO_MAX_DETS ? YOLO_MAX_DETS : display->count;
    for (int i = 0; i < meta->count; ++i) {
        const yolo_det_t *det = &display->dets[i];
        yolo_event_object_t *object = &meta->objects[i];
        object->cls = det->cls;
        snprintf(object->name, sizeof(object->name), "%s",
                 det->cls >= 0 && det->cls < g_yolo.classes.count ? g_yolo.classes.names[det->cls] : "obj");
        object->conf = det->conf;
        object->x1 = det->x1; object->y1 = det->y1;
        object->x2 = det->x2; object->y2 = det->y2;
    }
    return 0;
}

static void yolo_record_result(const yolo_job_t *job, const yolo_result_t *result)
{
    if (result->count < 0) return;
    yolo_event_meta_t metadata = job->metadata;
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);
    unsigned long long unix_ms = (unsigned long long)wall.tv_sec * 1000u + wall.tv_nsec / 1000000u;
    if (yolo_event_describe_result(&metadata, result, yolo_now_us() / 1000u, unix_ms) == 0) {
        follow_submit(&metadata);
        if (yolo_events_due(result->count, yolo_now_us() / 1000u))
            yolo_events_submit_meta(&metadata);
    }
}

static void *yolo_render_task(void *arg)
{
    (void)arg;
    (void)pthread_setname_np(pthread_self(), "yolo_render");
    frame_ring_t *ring = yolo_ring();
    while (g_yolo.running && g_yolo.pool_running) {
        if (!ring) { usleep(200000); continue; }   /* 防御：video 未初始化 */

        /* 事件驱动：等推理结果入队（40ms 超时仅用于退出检查） */
        yolo_result_t item;
        if (yolo_result_pop_latest(&item, 40) != 0) continue;
        /* 结果须晚于已渲染 seq（渲染 seq 严格递增，绝不回退） */
        if (item.seq <= g_yolo.next_seq) continue;

        unsigned long long seq = item.seq;
        int w = 0, h = 0;
        ring_buf_t rgbb = { 0 };
        frame_ring_lock(ring);
        /* Rendering owns RGB until it explicitly returns the buffer below.
           The slot may be cleared or reused during a camera restart. */
        frame_slot_t *s = yolo_slot(ring, seq);
        if (yolo_render_take_rgb_locked(ring, seq, &rgbb, &w, &h) != 0) {
            frame_ring_unlock(ring);
            continue;
        }
        g_yolo.next_seq = seq;
        unsigned long long tracking_capture_ms = s->capture_ms;
        frame_ring_unlock(ring);

        yolo_result_t disp = item;
        disp.w = w; disp.h = h;
        disp.count = item.count < 0 ? 0 : item.count > YOLO_MAX_DETS ? YOLO_MAX_DETS : item.count;

        /* 检测框只画一次：浏览器端 canvas 叠加（monitor.js overlayDraw）。
           服务端不再把框烧进 RGB —— 这一帧没有消费者（录像走相机原始 JPEG、
           直播走原始 JPEG 透传），每帧画一遍纯属浪费 CPU。 */
        unsigned long long object_ids[YOLO_MAX_DETS];
        (void)image_tracking_update(&disp, &g_yolo.classes, tracking_capture_ms, object_ids);
        /* Keep velocity estimation on raw detections. */
        yolo_smooth(&g_yolo.prev, &disp);
        g_yolo.prev = disp;
        snap_result(&disp);

        frame_ring_lock(ring);
        /* 槽可能已被退让清空（相机重启）：校验 seq 后提交，否则缓冲回池 */
        if (!ring->quiescing && s->seq == seq && s->rgb_done)
            frame_ring_display_commit_locked(ring, s);
        if (rgbb.buf)
            frame_ring_buf_put_locked(ring, FRAME_RING_POOL_RGB, rgbb.buf, rgbb.cap);
        frame_ring_unlock(ring);

        /* 交接开销：后处理完成 → 本次显示提交；以及解码结束 → 显示提交 */
        if (item.post_done_us) {
            unsigned long long now_us = yolo_now_us();
            unsigned long long wait = (now_us - item.post_done_us) / 1000u;
            unsigned long long d2d = item.decode_end_us
                                     ? (now_us - item.decode_end_us) / 1000u : 0;
            static unsigned long long last_log_us, wait_sum, d2d_sum, samples;
            wait_sum += wait; d2d_sum += d2d; samples++;
            if (now_us - last_log_us >= 10000000u) {
                last_log_us = now_us;
                if (samples) {
                    LOG_INFO("yolo handoff ms: post_to_display=%llu decode_to_display=%llu samples=%llu",
                             wait_sum / samples, d2d_sum / samples, samples);
                    wait_sum = d2d_sum = samples = 0;
                }
            }
        }

        /* Capture and the bounded result queue already pace this consumer.
           Do not hold a completed frame behind an additional 33 ms timer. */
    }
    return NULL;
}

/* ---- 模型加载与生命周期 ---- */

/* 模型文件读入内存；失败返回 -1 */
static int load_model_file(const char *path, unsigned char **buf_out, size_t *len_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long sz = ftell(fp);
    if (sz <= 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return -1; }
    fclose(fp);
    *buf_out = buf;
    *len_out = (size_t)sz;
    return 0;
}

static int yolo_model_io_valid(rknn_context probe)
{
    rknn_input_output_num io_num = {0};
    if (rknn_query(probe, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != RKNN_SUCC ||
        io_num.n_input != 1 || (io_num.n_output != 1 && io_num.n_output != 2)) {
        LOG_ERROR("yolo: model inputs=%u outputs=%u, expected one RGB input and one or two detection outputs",
                  io_num.n_input, io_num.n_output);
        return -1;
    }
    if (io_num.n_output == 2) {
        /* YOLO26 one2one：一路类别 (1,80,A)、一路框 (1,4,A)，无 NMS */
        int have_scores = 0, have_boxes = 0;
        for (unsigned i = 0; i < 2; i++) {
            rknn_tensor_attr attr;
            memset(&attr, 0, sizeof(attr));
            attr.index = i;
            if (rknn_query(probe, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr)) != RKNN_SUCC) continue;
            if (attr.n_dims != 3) continue;
            unsigned d1 = attr.dims[1], d2 = attr.dims[2];
            if (d1 == 4 || d2 == 4) have_boxes = 1;
            else if ((d1 >= 1 && d1 <= YOLO_MAX_CLASSES) || (d2 >= 1 && d2 <= YOLO_MAX_CLASSES)) have_scores = 1;
        }
        if (!have_scores || !have_boxes) {
            LOG_ERROR("yolo: two-output model is not a YOLO26 one2one head (scores=%d boxes=%d)",
                      have_scores, have_boxes);
            return -1;
        }
    }
    return 0;
}

int rknn_yolo_validate_model(const char *path)
{
    unsigned char *model = NULL;
    size_t length = 0;
    rknn_context probe = 0;
    if (!path || !*path || load_model_file(path, &model, &length) < 0) return -1;
    int result = -1;
    if (length <= UINT32_MAX && rknn_init(&probe, model, (uint32_t)length, 0, NULL) == RKNN_SUCC)
        result = yolo_model_io_valid(probe);
    if (probe) rknn_destroy(probe);
    free(model);
    return result;
}

/* 本工程只针对 Radxa ROCK 4D / RK3576：NPU 固定双核（Core0/Core1）。
   原先按设备树 compatible 推断核数的平台表已移除，不再做跨平台自适应。 */
#define NPU_CORE_COUNT 2

const char *npu_platform_name(void) { return "rk3576"; }

int npu_core_count(void) { return NPU_CORE_COUNT; }

void rknn_yolo_default_model_path(char *out, size_t size)
{
    if (out && size) snprintf(out, size, "config/yolo26.rknn");
}

/* 创建一个推理上下文：rknn_init + 绑定 NPU 核 + 查输出属性 + 分配输入缓冲 */
static int yolo_worker_setup(yolo_worker_t *wk, int idx)
{
    wk->ctx = 0;
    wk->in_buf = NULL;
    memset(&wk->out_attr, 0, sizeof(wk->out_attr));

    int ret = rknn_init(&wk->ctx, g_yolo.model, (uint32_t)g_yolo.model_len, 0, NULL);
    if (ret != RKNN_SUCC) {
        LOG_ERROR("yolo: worker rknn_init failed (%d)", ret);
        wk->ctx = 0;
        return -1;
    }
    /* RK3576 固定绑定：worker0 -> NPU Core0，worker1 -> NPU Core1 */
    int core = idx & 1;
    rknn_core_mask mask = (rknn_core_mask)(1u << core);
    if (rknn_set_core_mask(wk->ctx, mask) != RKNN_SUCC) {
        LOG_ERROR("yolo: worker %d set_core_mask(core %d) failed", idx, core);
        mask = RKNN_NPU_CORE_0;
    }
    LOG_INFO("yolo: worker %d bound to NPU core %d (mask 0x%x)", idx, core, (unsigned)mask);

    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    if (rknn_query(wk->ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != RKNN_SUCC ||
        io_num.n_output < 1 || io_num.n_output > 2) {
        LOG_ERROR("yolo: worker unexpected output count %u", io_num.n_output);
        return -1;
    }
    wk->out_count = io_num.n_output;
    for (unsigned i = 0; i < io_num.n_output; i++) {
        wk->out_attr[i].index = i;
        if (rknn_query(wk->ctx, RKNN_QUERY_OUTPUT_ATTR, &wk->out_attr[i],
                       sizeof(wk->out_attr[i])) != RKNN_SUCC) {
            LOG_ERROR("yolo: worker rknn_query OUTPUT_ATTR[%u] failed", i);
            return -1;
        }
    }
    wk->in_buf = malloc((size_t)g_yolo.in_w * g_yolo.in_h * 3);
    if (!wk->in_buf) {
        LOG_ERROR("yolo: worker input buffer alloc failed");
        return -1;
    }
    return 0;
}

/* 释放模型与全部 worker 资源（停池后调用） */
static void yolo_free_workers(void)
{
    for (int i = 0; i < g_yolo.nthreads; i++) {
        yolo_worker_t *wk = &g_yolo.workers[i];
        if (wk->in_buf) { free(wk->in_buf); wk->in_buf = NULL; }
        if (wk->ctx) { rknn_destroy(wk->ctx); wk->ctx = 0; }
    }
    free(g_yolo.model);
    g_yolo.model = NULL;
}

/* 停止推理池：置 pool_running=0 → 唤醒各线程 → join → 清理队列/模型。
   可安全重入（池未启动时 n_created=0，各清理为幂等操作）。
   在途任务：worker 推理中检出停池后作废（rgb 回池 + 摘除 claim），
   队列残余任务由本函数统一作废 */
static void yolo_stop_pool(void)
{
    /* Let a failing report writer release blocked producers before worker joins.
       Healthy storage still accepts and drains each in-flight inference. */
    if (g_yolo.events_initialized) yolo_events_prepare_stop();
    frame_ring_t *ring = yolo_ring();
    pthread_mutex_lock(&g_yolo.q_lock);
    g_yolo.enabled = 0;
    g_yolo.pool_running = 0;
    pthread_cond_broadcast(&g_yolo.q_not_empty);
    pthread_cond_broadcast(&g_yolo.q_not_full);
    pthread_mutex_unlock(&g_yolo.q_lock);
    /* raw 释放规则切回编码路径，并取消采集订阅等待，避免停池时等无帧超时。 */
    if (ring) {
        frame_ring_set_yolo_active(ring, 0);
        frame_ring_signal(ring);
    }
    /* Pair shutdown with the waiters' mutexes to close the check/wait window. */
    pthread_mutex_lock(&g_yolo.r_lock);
    pthread_cond_broadcast(&g_yolo.r_not_empty);
    pthread_cond_broadcast(&g_yolo.r_not_full);
    pthread_mutex_unlock(&g_yolo.r_lock);

    /* 解码线程先收：它会把在途帧投递/作废掉，再等 worker 退出 */
    yolo_decode_stop();

    for (int i = 0; i < g_yolo.n_created; i++)
        pthread_join(g_yolo.w_tid[i], NULL);
    g_yolo.n_created = 0;
    if (g_yolo.render_tid) {
        pthread_join(g_yolo.render_tid, NULL);
        g_yolo.render_tid = 0;
    }

    /* The producer may still be decoding a claimed raw frame. Let it return
       that frame before resetting queues and the shared sequence cursor. */
    pthread_mutex_lock(&g_producer_mutex);
    /* 释放队列中未被消费的任务（已消费槽位 rgb 已置 NULL） */
    for (int i = 0; i < YOLO_QUEUE_CAP; i++) {
        if (g_yolo.jobs[i].rgb || g_yolo.jobs[i].annot_nv12) {
            yolo_job_t job = g_yolo.jobs[i];
            g_yolo.jobs[i].rgb = NULL;
            g_yolo.jobs[i].annot_nv12 = NULL;
            yolo_job_discard(&job);
        }
    }
    g_yolo.q_head = g_yolo.q_tail = g_yolo.q_count = 0;

    /* 结果队列条目为纯元数据（无缓冲/claim），直接清空即可 */
    g_yolo.r_head = g_yolo.r_tail = g_yolo.r_count = 0;

    /* 清空快照与渲染状态（新池从干净状态开始；next_seq 保留跨 reload 连续） */
    pthread_mutex_lock(&g_yolo.result_mutex);
    memset(&g_yolo.result, 0, sizeof(g_yolo.result));
    pthread_mutex_unlock(&g_yolo.result_mutex);
    memset(&g_yolo.prev, 0, sizeof(g_yolo.prev));
    image_tracking_reset();
    g_yolo.last_seq = 0;

    yolo_free_workers();
    g_yolo.enabled = 0;
    pthread_mutex_unlock(&g_producer_mutex);
}

/* 启动推理池：重读配置 → 加载模型 → 建 worker → 启动线程。
   YOLO 为必要流程：任一失败即返回 -1 直接报错（启动路径整体失败退出；
   热重载路径由 HTTP 接口向调用方报告），enabled=0 仅供失败后运行态空转 */
static int yolo_start_pool(void)
{
    app_ctx_t *app = g_yolo.app;
    struct app_config_t *cfg = app->cfg;

    /* 重读运行时配置（支持热重载） */
    g_yolo.conf = cfg->yolo_conf > 0 ? cfg->yolo_conf : 0.25f;
    g_yolo.nms  = cfg->yolo_nms  > 0 ? cfg->yolo_nms  : 0.45f;
    /* RK3576：NPU 双核，固定两个推理线程，一核一个 */
    g_yolo.nthreads = NPU_CORE_COUNT;
    __atomic_store_n(&g_yolo.active_workers, NPU_CORE_COUNT, __ATOMIC_RELAXED);

    safe_strncpy(g_yolo.names_path, sizeof(g_yolo.names_path),
                 cfg->yolo_names[0] ? cfg->yolo_names : "config/coco.names");

    char model_path[256];
    char previous_default[256], previous_relative[256];
    snprintf(previous_default, sizeof(previous_default), "config/models/%s/yolo26.rknn", npu_platform_name());
    snprintf(previous_relative, sizeof(previous_relative), "./config/models/%s/yolo26.rknn", npu_platform_name());
    if (cfg->yolo_model[0] && strcmp(cfg->yolo_model, previous_default) &&
        strcmp(cfg->yolo_model, previous_relative) && strcmp(cfg->yolo_model, "./config/yolo26.rknn"))
        safe_strncpy(model_path, sizeof(model_path), cfg->yolo_model);
    else
        rknn_yolo_default_model_path(model_path, sizeof(model_path));
    if (load_model_file(model_path, &g_yolo.model, &g_yolo.model_len) < 0) {
        LOG_ERROR("yolo: model '%s' not found — YOLO unavailable (no YOLO)", model_path);
        g_yolo.enabled = 0;
        return -1;
    }

    /* 用第一个上下文获取输入尺寸（静态模型）与输出数校验 */
    rknn_context probe = 0;
    if (rknn_init(&probe, g_yolo.model, (uint32_t)g_yolo.model_len, 0, NULL) != RKNN_SUCC) {
        LOG_ERROR("yolo: rknn_init failed: model incompatible or NPU unavailable — YOLO unavailable");
        yolo_free_workers();
        g_yolo.enabled = 0;
        return -1;
    }
    if (yolo_model_io_valid(probe)) {
        rknn_destroy(probe);
        yolo_free_workers();
        g_yolo.enabled = 0;
        return -1;
    }
    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    if (rknn_query(probe, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr)) != RKNN_SUCC) {
        LOG_ERROR("yolo: rknn_query INPUT_ATTR failed — YOLO unavailable");
        rknn_destroy(probe);
        yolo_free_workers();
        g_yolo.enabled = 0;
        return -1;
    }
    rknn_destroy(probe);

    /* 输入尺寸：优先用模型静态尺寸；dims 全 0（动态模型）时用配置值 */
    if (in_attr.fmt == RKNN_TENSOR_NCHW && in_attr.n_dims >= 4) {
        g_yolo.in_w = (int)in_attr.dims[3];
        g_yolo.in_h = (int)in_attr.dims[2];
    } else if (in_attr.fmt == RKNN_TENSOR_NHWC && in_attr.n_dims >= 4) {
        g_yolo.in_w = (int)in_attr.dims[2];
        g_yolo.in_h = (int)in_attr.dims[1];
    }
    if (g_yolo.in_w <= 0 || g_yolo.in_h <= 0) {
        int s = cfg->yolo_input_size > 0 ? cfg->yolo_input_size : 640;
        g_yolo.in_w = s;
        g_yolo.in_h = s;
    }
    /* RGA 硬件预处理（letterbox + resize 一趟完成）是必需组件：
       缺驱动/库直接判为不可用，不做软件回退。 */
    if (!g_yolo.rga) {
        if (!rga_preprocess_available() || !(g_yolo.rga = rga_preprocess_open(g_yolo.in_w, g_yolo.in_h))) {
            LOG_ERROR("yolo: RGA 预处理不可用（/dev/rga 或 librga 缺失）；全硬件通路要求 RGA，停止");
            yolo_free_workers();
            g_yolo.enabled = 0;
            return -1;
        }
    }
    LOG_INFO("yolo: 预处理使用 RGA 硬件（NV12 → %dx%d letterbox），CPU 缩放与转换均已移除",
             g_yolo.in_w, g_yolo.in_h);

    LOG_INFO("yolo: input %s, quantization %s, scale %.8g zp %d",
             get_type_string(in_attr.type), get_qnt_type_string(in_attr.qnt_type),
             in_attr.scale, in_attr.zp);

    /* 类别名表：官方 COCO 格式文件（缺失/为空时回退内置 80 类） */
    int ncls = yolo_classes_load(g_yolo.names_path, &g_yolo.classes);

    /* 每个工作线程独立上下文，绑定独立 NPU 核 */
    for (int i = 0; i < g_yolo.nthreads; i++) {
        if (yolo_worker_setup(&g_yolo.workers[i], i) < 0) {
            yolo_free_workers();
            g_yolo.enabled = 0;
            LOG_ERROR("yolo: pool start failed — YOLO unavailable (no YOLO)");
            return -1;
        }
    }

    /* 启动推理工作线程池 + 按序渲染线程 */
    g_yolo.pool_running = 1;
    /* 解码线程先起来：worker 依赖解码投递的任务 */
    if (yolo_decode_start() < 0) {
        yolo_stop_pool();
        return -1;
    }
    for (int i = 0; i < g_yolo.nthreads; i++) {
        if (app_thread_create(&g_yolo.w_tid[i], "yolo_infer", yolo_worker_task,
                              (void *)(intptr_t)i, 0) != 0) {
            LOG_ERROR("yolo: worker thread %d create failed", i);
            yolo_stop_pool();
            return -1;
        }
        g_yolo.n_created = i + 1;
    }
    if (app_thread_create(&g_yolo.render_tid, "yolo_render", yolo_render_task, NULL, 0) != 0) {
        LOG_ERROR("yolo: render thread create failed (annotated stream stalled)");
        g_yolo.render_tid = 0;
        yolo_stop_pool();
        return -1;
    }
    g_yolo.enabled = 1;
    /* 环内 raw 释放规则切回推理路径（infer_done 门槛） */
    if (yolo_ring()) frame_ring_set_yolo_active(yolo_ring(), 1);
    LOG_INFO("yolo: model '%s' loaded, classes '%s' (%d), input %dx%d, "
             "1 detection output, fixed threads %d/%d (NPU core i%%%d), conf %.2f nms %.2f",
             model_path, g_yolo.names_path, ncls, g_yolo.in_w, g_yolo.in_h,
             rknn_yolo_active_workers(), g_yolo.nthreads, NPU_CORE_COUNT, g_yolo.conf, g_yolo.nms);
    return 0;
}

/* 热重载：停止旧池 → 重读配置重建。由前端上传模型/标签文件、修改 YOLO 参数触发。
   失败返回 -1（enabled=0）：调用方（HTTP 接口）向客户端直接报告 reload 失败 */
int rknn_yolo_reload(void)
{
    pthread_mutex_lock(&g_reload_mutex);
    if (!g_yolo.running) {
        pthread_mutex_unlock(&g_reload_mutex);
        return -1;
    }
    atomic_store(&g_reloading, 1);
    LOG_INFO("yolo: reloading inference pool...");
    yolo_stop_pool();
    if (g_yolo.events_initialized) {
        yolo_events_destroy();
        g_yolo.events_initialized = 0;
    }
    if (yolo_events_init(NULL) == 0) g_yolo.events_initialized = 1;
    else LOG_ERROR("yolo: detection report saving unavailable after reload");
    yolo_start_pool();
    atomic_store(&g_reloading, 0);
    pthread_mutex_unlock(&g_reload_mutex);
    return g_yolo.enabled ? 0 : -1;
}

int rknn_yolo_init(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    if (!app || !app->cfg) {
        LOG_ERROR("yolo: init without config");
        return -1;
    }
    memset(&g_yolo, 0, sizeof(g_yolo));
    g_yolo.app = app;
    g_yolo.running = 1;
    pthread_mutex_init(&g_yolo.result_mutex, NULL);
    pthread_mutex_init(&g_yolo.q_lock, NULL);
    pthread_cond_init(&g_yolo.q_not_empty, NULL);
    pthread_cond_init(&g_yolo.q_not_full, NULL);
    pthread_mutex_init(&g_yolo.r_lock, NULL);
    pthread_cond_init(&g_yolo.r_not_empty, NULL);
    pthread_cond_init(&g_yolo.r_not_full, NULL);
    pthread_mutex_init(&g_decode.lock, NULL);
    pthread_cond_init(&g_decode.not_empty, NULL);
    pthread_cond_init(&g_decode.not_full, NULL);

    /* 解码固定走 libjpeg-turbo 软解。libjpeg 是链接期依赖，探测只用于状态上报。 */
    atomic_store(&g_yolo.decoder_ready, jpeg_decoder_available());
    LOG_INFO("yolo: JPEG decode %s", atomic_load(&g_yolo.decoder_ready)
             ? "libjpeg-turbo (software)"
             : "unavailable");
    if (yolo_events_init(NULL) == 0) g_yolo.events_initialized = 1;
    else LOG_ERROR("yolo: detection report saving unavailable; live inference remains enabled");
    /* YOLO 为必要流程：无 YOLO（模型缺失/NPU 不可用）直接报错，服务启动失败 */
    if (yolo_start_pool() < 0) {
        LOG_ERROR("yolo: mandatory inference pool failed to start — no YOLO, aborting startup");
        rknn_yolo_destroy(arg);
        return -1;
    }
    return 0;
}

void rknn_yolo_destroy(void *arg)
{
    (void)arg;
    g_yolo.running = 0;
    yolo_stop_pool();
    if (g_yolo.rga) { rga_preprocess_close(g_yolo.rga); g_yolo.rga = NULL; }
    if (g_yolo.events_initialized) {
        yolo_events_destroy();
        g_yolo.events_initialized = 0;
    }

    pthread_mutex_destroy(&g_yolo.q_lock);
    pthread_cond_destroy(&g_yolo.q_not_empty);
    pthread_cond_destroy(&g_yolo.q_not_full);
    pthread_mutex_destroy(&g_yolo.r_lock);
    pthread_cond_destroy(&g_yolo.r_not_empty);
    pthread_cond_destroy(&g_yolo.r_not_full);
    pthread_mutex_destroy(&g_yolo.result_mutex);
    g_yolo.enabled = 0;
}

/* ---- YOLO 采集线程（main 模块框架 task）：等新帧 → claim → 解码 → 投递队列 ---- */
int rknn_yolo_decoder_ready(void)
{
    return atomic_load(&g_yolo.decoder_ready);
}

void *rknn_yolo_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    if (!app) return NULL;
    (void)pthread_setname_np(pthread_self(), "yolo");
    if (watchdog_register_thread(pthread_self(), "yolo", 5, 3) < 0) {
        app->fatal = 1;
        app->running = 0;
        return NULL;
    }

    frame_ring_t *ring = yolo_ring();
    frame_subscription_t capture;
    if (ring) frame_ring_subscribe(ring, &capture, FRAME_TOPIC_CAPTURED);

    while (app->running && g_yolo.running) {
        watchdog_feed_self("yolo");

        /* 未启用 / 池未运行（重载中）/ video 未初始化：空转喂狗，不占推理资源。
           重载后 enabled 恢复时自动继续采集 */
        if (!ring || !g_yolo.enabled || !g_yolo.pool_running) { usleep(200000); continue; }

        /* 解码队列满就在这里背压，避免把 claim 的槽位堆在队列里等 */
        if (yolo_decode_wait_room() < 0) { usleep(1000); continue; }

        pthread_mutex_lock(&g_producer_mutex);
        if (!g_yolo.enabled || !g_yolo.pool_running) {
            pthread_mutex_unlock(&g_producer_mutex);
            continue;
        }

        /* 订阅采集主题：发布即醒；取消代次覆盖状态检查到 wait 之间的停池窗口。
           2s 超时仅用于周期性检查退出/热插拔，不控制处理间隔。 */
        if (!frame_ring_wait(&capture, g_yolo.last_seq, 2000)) {
            pthread_mutex_unlock(&g_producer_mutex);
            continue;
        }

        /* 按序 claim 下一个未处理帧的 raw（与旧轮询版语义一致：每帧都解码，
           队列背压即限速）。槽已被编码回退消费/退让清空时顺延跳过。
           claim 随任务移交 worker（写槽时 unclaim），任务在途期间槽位
           被 claims 钉住，不会被采集复用，raw 也不会被释放 */
        frame_ring_lock(ring);
        /* 在途上限：解码并行后生产速率追平消费，队列一旦填满就不再排空（实测
           端到端 169 ms，单线程解码时 65 ms）。把未完成的 claim 压到两帧以内，
           延迟回到两个帧周期量级；单帧处理 15 ms 远小于 33 ms 帧周期，不丢帧。 */
        if (yolo_inflight_locked(ring) >= YOLO_MAX_INFLIGHT) {
            frame_ring_unlock(ring);
            pthread_mutex_unlock(&g_producer_mutex);
            usleep(2000);
            continue;
        }
        frame_slot_t *s = NULL;
        while (g_yolo.last_seq < ring->produce_seq) {
            s = frame_ring_infer_claim_locked(ring, g_yolo.last_seq + 1);
            if (s) break;
            g_yolo.last_seq++;
        }
        if (!s) {
            frame_ring_unlock(ring);
            pthread_mutex_unlock(&g_producer_mutex);
            continue;
        }
        unsigned long long seq = s->seq;
        unsigned long long capture_ms = s->capture_ms;
        unsigned long long deliver_ms = s->deliver_ms;
        yolo_event_meta_t metadata;
        yolo_event_capture_meta_locked(s, &metadata);
        int fmt = s->fmt, w = s->w, h = s->h;
        unsigned char *raw = s->raw.buf;
        size_t raw_len = s->raw.len;
        frame_ring_unlock(ring);

        /* 解码结果只以 NV12 dma-buf 形式交给 RGA；解码本身在 decode 线程里，
           这里只投递。claim 随任务一路走到 worker 写槽，在途期间钉住槽位。 */
        g_yolo.last_seq = seq;

        int supported = raw && raw_len && w > 0 && h > 0;
        if (supported && fmt == FRAME_RING_FMT_MJPEG) {
            decode_item_t item;
            memset(&item, 0, sizeof(item));
            item.raw = raw;
            item.raw_len = raw_len;
            item.job.w = w;
            item.job.h = h;
            item.job.seq = seq;
            item.job.capture_ms = capture_ms;
            item.job.deliver_ms = deliver_ms;
            item.job.decode_start_us = yolo_now_us();
            item.job.metadata = metadata;
            /* wait_room 已确认空位，投递不阻塞 */
            yolo_decode_submit(&item);
            pthread_mutex_unlock(&g_producer_mutex);
            continue;
        }

        /* 无帧内容或格式不支持：摘 claim（解码未发生，计 infer_dropped） */
        frame_ring_lock(ring);
        frame_ring_infer_unclaim_locked(ring, s, 0);
        frame_ring_unlock(ring);
        pthread_mutex_unlock(&g_producer_mutex);
        if (supported) {
            /* YUYV 需要 RGA 直接吃 4:2:2，当前通路只实现 NV12 */
            LOG_ERROR("yolo: 取流只支持 MJPEG/NV12 输入，当前帧格式不支持");
            atomic_store(&g_yolo.decoder_ready, 0);
            LOG_ERROR("yolo: MJPEG capture requires a working JPEG decoder; stopping");
            app->fatal = 1;
            app->running = 0;
            break;
        }
        usleep(1000);   /* 坏帧退避，避免忙转 */
    }
    if (app->running) app->fatal = 1;
    app->running = 0;
    watchdog_unregister_self("yolo");
    return NULL;
}
