/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "http/http.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <linux/videodev2.h>
#include <sys/epoll.h>
#include <poll.h>
#include <pthread.h>
#include "video/video_stream.h"
#include "video/frame_ring.h"
#include "core/log.h"
#include "core/common.h"
#include "core/epoll_util.h"
#include "watchdog/watchdog.h"
#include "yolo/rknn_yolo.h"

/* 推流客户端空闲超时（秒）：客户端不读 / 网络黑洞时释放连接与线程 */
#define VIDEO_CLIENT_IDLE_TIMEOUT 30

typedef struct {
    _Atomic(int)       restart_req;  /* 请求 worker 用新参数重新初始化设备 */
    int                running;
    pthread_mutex_t    cfg_mutex;    /* 保护 device / width / height */
    frame_ring_t       ring;         /* 视频帧环形队列：采集侧零拷贝发布 */
    char               device[128];
    char               camera_label[FRAME_RING_CAMERA_LEN]; /* Immutable during each capture session. */
    int                width, height;
    int                pixfmt;   /* 实际像素格式（V4L2_PIX_FMT_*） */
    int                fd;
    struct { void *start; size_t length; } *buffers;
    int                nbuffers;
    int                focus_auto_supported, focus_auto_writable;
    int                focus_absolute_supported;
    unsigned long long focus_auto_check_ms, focus_error_log_ms;
    app_ctx_t         *app;          /* 运行上下文（读 running / cfg） */
} video_ctx_t;

static video_ctx_t *g_ctx = NULL;

/* 延迟 QBUF 释放回调：环在 raw 消费完成后归还驱动缓冲（零拷贝采集核心）。
   在环锁内调用（ioctl 微秒级），不得获取其他锁；持续失败时降频日志 */
static void raw_release_cb(void *arg, int vbuf_index)
{
    video_ctx_t *vs = (video_ctx_t *)arg;
    static int fail_cnt = 0;
    struct v4l2_buffer buf = {0};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = vbuf_index;
    if (ioctl(vs->fd, VIDIOC_QBUF, &buf) < 0 && (fail_cnt++ % 100) == 0)
        LOG_ERROR("video_stream: deferred QBUF failed: %s", strerror(errno));
}

/* ---- 内部函数 ---- */

typedef struct {
    int valid, position, autofocus;
    unsigned long long sample_ms;
} video_focus_sample_t;

static unsigned long long video_monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000u + now.tv_nsec / 1000000u;
}

static int video_focus_query(int fd, unsigned int id, int *writable)
{
    struct v4l2_queryctrl control = {.id = id};
    if (ioctl(fd, VIDIOC_QUERYCTRL, &control) < 0 ||
        (control.flags & V4L2_CTRL_FLAG_DISABLED)) return 0;
    if (writable) *writable = !(control.flags & (V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_INACTIVE));
    return 1;
}

static void video_focus_init(video_ctx_t *vs)
{
    vs->focus_auto_writable = 0;
    vs->focus_auto_supported = video_focus_query(vs->fd, V4L2_CID_FOCUS_AUTO, &vs->focus_auto_writable);
    vs->focus_absolute_supported = video_focus_query(vs->fd, V4L2_CID_FOCUS_ABSOLUTE, NULL);
    vs->focus_auto_check_ms = video_monotonic_ms();
    vs->focus_error_log_ms = 0;
    if (vs->focus_auto_supported && vs->focus_auto_writable) {
        struct v4l2_control automatic = {.id = V4L2_CID_FOCUS_AUTO, .value = 1};
        if (ioctl(vs->fd, VIDIOC_S_CTRL, &automatic) < 0)
            LOG_ERROR("video_stream: unable to enable continuous autofocus: %s", strerror(errno));
    }
}

/* Keep the negotiated capture cadence in low light. Automatic exposure and
 * autofocus remain enabled; only permission to extend the frame interval is
 * disabled. UVC exposes this as "exposure_dynamic_framerate". */
static void video_capture_timing_init(video_ctx_t *vs)
{
    int writable = 0;
    if (!video_focus_query(vs->fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY, &writable) || !writable) return;
    struct v4l2_control priority = {.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY, .value = 0};
    if (ioctl(vs->fd, VIDIOC_S_CTRL, &priority) < 0)
        LOG_ERROR("video_stream: unable to disable dynamic frame intervals: %s", strerror(errno));
}

/* Measured on the UGREEN camera: both G_CTRL calls take about 0.18 ms.
 * Keep them outside the frame ring lock. Their completion timestamp explicitly
 * distinguishes this lens sample from the sensor's earlier exposure timestamp. */
static void video_focus_sample(video_ctx_t *vs, video_focus_sample_t *sample)
{
    memset(sample, 0, sizeof(*sample));
    sample->autofocus = -1;
    struct v4l2_control automatic = {.id = V4L2_CID_FOCUS_AUTO};
    if (vs->focus_auto_supported && ioctl(vs->fd, VIDIOC_G_CTRL, &automatic) == 0)
        sample->autofocus = !!automatic.value;
    unsigned long long now = video_monotonic_ms();
    if (vs->focus_auto_supported && vs->focus_auto_writable &&
        now - vs->focus_auto_check_ms >= 1000) {
        vs->focus_auto_check_ms = now;
        if (sample->autofocus != 1) {
            automatic.value = 1;
            if (ioctl(vs->fd, VIDIOC_S_CTRL, &automatic) == 0) {
                if (ioctl(vs->fd, VIDIOC_G_CTRL, &automatic) == 0)
                    sample->autofocus = !!automatic.value;
            } else if (!vs->focus_error_log_ms || now - vs->focus_error_log_ms >= 30000) {
                vs->focus_error_log_ms = now;
                LOG_ERROR("video_stream: unable to restore continuous autofocus: %s", strerror(errno));
            }
        }
    }
    struct v4l2_control position = {.id = V4L2_CID_FOCUS_ABSOLUTE};
    if (vs->focus_absolute_supported && ioctl(vs->fd, VIDIOC_G_CTRL, &position) == 0) {
        sample->valid = 1;
        sample->position = position.value;
    }
    sample->sample_ms = video_monotonic_ms();
}

static int init_video_device(video_ctx_t *vs, int verbose)
{
    /* 设备初始化期间持续喂狗：部分 V4L2 驱动的 open/ioctl 可能较慢，
       整段初始化若耗时过长会被看门狗误判为 video 线程卡死。 */
    watchdog_feed_self("video");

    vs->fd = open(vs->device, O_RDWR | O_NONBLOCK, 0);
    if (vs->fd < 0) {
        /* 相机未连接是常见场景：重试时静默，仅在状态切换时详细记录，避免刷屏 */
        if (verbose)
            LOG_ERROR("video_stream: open %s failed: %s", vs->device, strerror(errno));
        return -1;
    }
    watchdog_feed_self("video");

    struct v4l2_capability cap = {0};
    if (ioctl(vs->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("video_stream: VIDIOC_QUERYCAP failed");
        goto fail_close;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        LOG_ERROR("video_stream: device %s is not video capture", vs->device);
        goto fail_close;
    }
    if (cap.card[0])
        snprintf(vs->camera_label, sizeof(vs->camera_label), "%.*s (%s)",
                 (int)sizeof(cap.card), (const char *)cap.card, vs->device);
    else
        snprintf(vs->camera_label, sizeof(vs->camera_label), "%s", vs->device);
    watchdog_feed_self("video");

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = vs->width;
    fmt.fmt.pix.height      = vs->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (ioctl(vs->fd, VIDIOC_S_FMT, &fmt) < 0) {
        /* MJPEG 失败，尝试 YUYV */
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (ioctl(vs->fd, VIDIOC_S_FMT, &fmt) < 0) {
            /* 仍失败则用 G_FMT 取驱动默认值 */
            LOG_ERROR("video_stream: S_FMT MJPG+YUYV failed, using driver default");
            memset(&fmt, 0, sizeof(fmt));
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl(vs->fd, VIDIOC_G_FMT, &fmt) < 0)
                goto fail_close;
        }
    }
    watchdog_feed_self("video");

    /* 记录实际使用的参数 */
    vs->width  = fmt.fmt.pix.width;
    vs->height = fmt.fmt.pix.height;
    vs->pixfmt = (int)fmt.fmt.pix.pixelformat;
    LOG_INFO("video_stream: fmt=%c%c%c%c %dx%d",
        (char)(fmt.fmt.pix.pixelformat & 0xFF),
        (char)((fmt.fmt.pix.pixelformat >> 8) & 0xFF),
        (char)((fmt.fmt.pix.pixelformat >> 16) & 0xFF),
        (char)((fmt.fmt.pix.pixelformat >> 24) & 0xFF),
        vs->width, vs->height);

    /* 帧率：配置 video_fps > 0 时设置驱动帧间隔（S_PARM），否则保持驱动默认 */
    if (vs->app && vs->app->cfg && vs->app->cfg->video_fps > 0) {
        struct v4l2_streamparm sp;
        memset(&sp, 0, sizeof(sp));
        sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        sp.parm.capture.timeperframe.numerator = 1;
        sp.parm.capture.timeperframe.denominator = vs->app->cfg->video_fps;
        if (ioctl(vs->fd, VIDIOC_S_PARM, &sp) == 0) {
            LOG_INFO("video_stream: requested fps=%d, negotiated fps=%u/%u",
                     vs->app->cfg->video_fps, sp.parm.capture.timeperframe.denominator,
                     sp.parm.capture.timeperframe.numerator);
        } else {
            LOG_ERROR("video_stream: S_PARM fps=%d failed, keeping driver default",
                      vs->app->cfg->video_fps);
        }
    }

    struct v4l2_streamparm actual = {0};
    actual.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(vs->fd, VIDIOC_G_PARM, &actual) == 0) {
        frame_ring_lock(&vs->ring);
        vs->ring.capture_fps_num = actual.parm.capture.timeperframe.denominator;
        vs->ring.capture_fps_den = actual.parm.capture.timeperframe.numerator;
        frame_ring_unlock(&vs->ring);
    }

    video_focus_init(vs);
    video_capture_timing_init(vs);

    struct v4l2_requestbuffers req = {0};
    /* 8 个驱动缓冲：零拷贝延迟 QBUF 模式下，缓冲占用 = 推理解码在途 +
       最新帧钉住 + 帧间隔，4 个缓冲不足会导致驱动侧静默丢帧 */
    req.count  = 8;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(vs->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        LOG_ERROR("video_stream: VIDIOC_REQBUFS failed");
        goto fail_close;
    }
    watchdog_feed_self("video");

    vs->buffers = calloc(req.count, sizeof(*vs->buffers));
    if (!vs->buffers) {
        LOG_ERROR("video_stream: calloc buffers failed");
        goto fail_close;
    }
    vs->nbuffers = req.count;

    for (int i = 0; i < vs->nbuffers; i++) {
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(vs->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("video_stream: VIDIOC_QUERYBUF failed");
            goto fail_buffers;
        }
        vs->buffers[i].length = buf.length;
        vs->buffers[i].start  = mmap(NULL, buf.length,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, vs->fd, buf.m.offset);
        if (vs->buffers[i].start == MAP_FAILED) {
            LOG_ERROR("video_stream: mmap failed");
            goto fail_buffers;
        }
        if (ioctl(vs->fd, VIDIOC_QBUF, &buf) < 0) {
            LOG_ERROR("video_stream: VIDIOC_QBUF failed");
            goto fail_buffers;
        }
        watchdog_feed_self("video");
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(vs->fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("video_stream: VIDIOC_STREAMON failed");
        goto fail_buffers;
    }
    watchdog_feed_self("video");
    return 0;

fail_buffers:
    for (int i = 0; i < vs->nbuffers; i++)
        if (vs->buffers[i].start && vs->buffers[i].start != MAP_FAILED)
            munmap(vs->buffers[i].start, vs->buffers[i].length);
    free(vs->buffers);
    vs->buffers  = NULL;
    vs->nbuffers = 0;
fail_close:
    close(vs->fd);
    vs->fd = -1;
    return -1;
}

static void deinit_video_device(video_ctx_t *vs)
{
    if (vs->fd < 0) return;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(vs->fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < vs->nbuffers; i++)
        if (vs->buffers[i].start && vs->buffers[i].start != MAP_FAILED)
            munmap(vs->buffers[i].start, vs->buffers[i].length);
    free(vs->buffers);
    vs->buffers  = NULL;
    vs->nbuffers = 0;
    close(vs->fd);
    vs->fd = -1;
}

/* ---- 公共接口 ---- */

int video_stream_init(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;

    video_ctx_t *vs = calloc(1, sizeof(*vs));
    if (!vs) return -1;
    pthread_mutex_init(&vs->cfg_mutex, NULL);
    frame_ring_init(&vs->ring);
    frame_ring_set_raw_release_cb(&vs->ring, raw_release_cb, vs);
    snprintf(vs->device, sizeof(vs->device), "%s", "/dev/video0");
    vs->width  = 640;
    vs->height = 480;
    vs->fd     = -1;
    vs->app    = app;

    if (app->cfg->video_device[0])
        snprintf(vs->device, sizeof(vs->device), "%s", app->cfg->video_device);
    if (app->cfg->video_width  > 0) vs->width  = app->cfg->video_width;
    if (app->cfg->video_height > 0) vs->height = app->cfg->video_height;
    vs->running = 1;
    g_ctx = vs;
    return 0;
}

void video_stream_shutdown(void *arg)
{
    (void)arg;
    if (!g_ctx) return;
    /* 仅置停止标志：环形队列随进程退出（推流客户端线程为 detached，
       可能仍在退出检查前短暂访问环，不能在此销毁） */
    g_ctx->running = 0;
    frame_ring_signal(&g_ctx->ring);
}

void *video_stream_task(void *arg)
{
    app_ctx_t *app = arg;
    (void)pthread_setname_np(pthread_self(), "video");
    if (watchdog_register_thread(pthread_self(), "video", 5, 3) < 0) {
        app->fatal = 1;
        app->running = 0;
        return NULL;
    }
    video_ctx_t *vs = g_ctx;
    if (!vs) goto out;

    int failed_before = 0;   /* 连续失败标记：只在状态切换时输出详细错误 */

    while (vs->app->running) {
        /* 会话开始：初始化设备（参数变更由 restart_req 触发重新进入本循环） */
        pthread_mutex_lock(&vs->cfg_mutex);
        int ok = (init_video_device(vs, !failed_before) == 0);
        /* Keep the negotiated session geometry with its frames while a new
           configuration is being requested by the HTTP thread. */
        int capture_width = vs->width, capture_height = vs->height;
        int capture_pixfmt = vs->pixfmt;
        pthread_mutex_unlock(&vs->cfg_mutex);

        if (!ok) {
            if (!failed_before) {
                LOG_ERROR("video_stream: device init failed, entering idle retry loop (watchdog still fed)");
                failed_before = 1;
            }
            /* 相机未连接：线程保持存活并持续喂狗，程序正常运行；
               每 5 秒重试一次初始化，便于热插拔后自动恢复 */
            int idle_ticks = 0;
            while (vs->app->running && !__atomic_load_n(&vs->restart_req, __ATOMIC_ACQUIRE)) {
                usleep(500000);
                watchdog_feed_self("video");
                if (++idle_ticks >= 10) break;
            }
            if (__atomic_load_n(&vs->restart_req, __ATOMIC_ACQUIRE))
                failed_before = 0;   /* 配置变更触发：下次失败重新详细记录 */
            __atomic_store_n(&vs->restart_req, 0, __ATOMIC_RELEASE);
            continue;
        }
        failed_before = 0;

        int epfd = epoll_create1(0);
        if (epfd < 0) {
            LOG_ERROR("video_stream: epoll_create1 failed");
            deinit_video_device(vs);
            __atomic_store_n(&vs->restart_req, 0, __ATOMIC_RELEASE);
            continue;
        }
        struct epoll_event vev = { .events = EPOLLIN | EPOLLERR, .data.fd = vs->fd };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, vs->fd, &vev) < 0) {
            LOG_ERROR("video_stream: epoll_ctl add device failed");
            close(epfd);
            deinit_video_device(vs);
            __atomic_store_n(&vs->restart_req, 0, __ATOMIC_RELEASE);
            continue;
        }
        LOG_INFO("video_stream: capturing from %s", vs->device);

        unsigned int last_vseq = 0;
        int have_vseq = 0;   /* 驱动侧丢帧统计（buf.sequence 缺口；会话级状态） */
        time_t last_summary = 0;   /* 环形队列分阶段计数器 60s 定期汇总 */
        while (vs->app->running && !__atomic_load_n(&vs->restart_req, __ATOMIC_ACQUIRE)) {
            struct epoll_event out;
            /* epoll_wait_feed：EINTR 重试并喂狗；无论是否有帧都保持喂狗
               （相机空闲/无数据时线程仍存活，避免误判卡死） */
            int ret = epoll_wait_feed(epfd, &out, 1, 500, "video");
            if (ret < 0) break;
            /* 60s 汇总必须放在每次循环都执行的位置：相机持续送帧时
               epoll 从不超时，放在 ret==0 分支会导致汇总永不打印 */
            time_t now = time(NULL);
            if (now - last_summary >= 60) {
                last_summary = now;
                frame_ring_stats_t st;
                frame_ring_stats(&vs->ring, &st);
                LOG_INFO("video_stream: ring 60s — produce %llu, capture_dropped %llu, "
                         "driver_dropped %llu, stall %llums, infer_dropped %llu, "
                         "render_skipped %llu, raw_pinned %d, pool rgb %d",
                         st.produce_seq, st.capture_dropped, st.driver_dropped,
                         st.capture_stall_ms, st.infer_dropped, st.render_skipped,
                         st.raw_pinned, st.pool_rgb);
            }
            if (ret == 0) continue;
            if (!(out.events & EPOLLIN)) continue;

            struct v4l2_buffer buf = {0};
            buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(vs->fd, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN || errno == EINTR) continue;
                LOG_ERROR("video_stream: VIDIOC_DQBUF failed");
                break;
            }

            /* 驱动侧丢帧：相邻 buf.sequence 的缺口（u32 回绕安全）。
               部分驱动不填 sequence（恒 0）时不计数 */
            if (have_vseq) {
                unsigned int gap = buf.sequence - last_vseq;
                if (gap > 1) {
                    frame_ring_lock(&vs->ring);
                    frame_ring_add_driver_dropped_locked(&vs->ring, gap - 1);
                    frame_ring_unlock(&vs->ring);
                }
            }
            last_vseq = buf.sequence;
            have_vseq = 1;

            if (buf.bytesused == 0 || buf.index >= vs->nbuffers) {
                /* 空帧：立即归还，不进环 */
                if (ioctl(vs->fd, VIDIOC_QBUF, &buf) < 0) {
                    LOG_ERROR("video_stream: VIDIOC_QBUF failed");
                    break;
                }
                continue;
            }

            video_focus_sample_t focus;
            video_focus_sample(vs, &focus);

            /* 零拷贝入环：mmap 指针直接进槽（不 memcpy），消费完成前不 QBUF。
               环无空闲槽（消费严重滞后）时丢弃本帧并立即归还驱动缓冲 */
            frame_ring_lock(&vs->ring);
            frame_slot_t *s = frame_ring_produce_slot_locked(&vs->ring);
            if (!s) {
                frame_ring_produce_drop_locked(&vs->ring);
                frame_ring_unlock(&vs->ring);
                if (ioctl(vs->fd, VIDIOC_QBUF, &buf) < 0) {
                    LOG_ERROR("video_stream: VIDIOC_QBUF failed");
                    break;
                }
                continue;
            }
            s->raw.buf        = vs->buffers[buf.index].start;
            s->raw.len        = buf.bytesused;
            s->raw.cap        = vs->buffers[buf.index].length;
            s->raw_present    = 1;
            s->raw_is_mmap    = 1;
            s->raw_vbuf_index = buf.index;
            s->focus_valid = focus.valid;
            s->focus_position = focus.position;
            s->autofocus = focus.autofocus;
            s->focus_sample_ms = focus.sample_ms;
            snprintf(s->camera, sizeof(s->camera), "%s", vs->camera_label);
            if ((buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
                struct timespec captured_now;
                clock_gettime(CLOCK_MONOTONIC, &captured_now);
                unsigned long long now_ms = (unsigned long long)captured_now.tv_sec * 1000u + captured_now.tv_nsec / 1000000u;
                unsigned long long capture_ms = (unsigned long long)buf.timestamp.tv_sec * 1000u + buf.timestamp.tv_usec / 1000u;
                if (capture_ms && capture_ms <= now_ms && now_ms - capture_ms < 5000)
                    s->capture_ms = capture_ms;
            }
            s->w = capture_width;
            s->h = capture_height;
            s->fmt = (capture_pixfmt == V4L2_PIX_FMT_MJPEG) ? FRAME_RING_FMT_MJPEG
                                                        : FRAME_RING_FMT_YUYV;
            frame_ring_produce_commit_locked(&vs->ring);
            frame_ring_unlock(&vs->ring);
            watchdog_feed_self("video");
        }

        /* 会话结束（重启/退出/错误）：退让等待锁外解码者释放引用（推理侧
           任务在途期间持有 claims），随后摘除全部 mmap 指针，
           STREAMOFF/munmap 才安全 */
        frame_ring_quiesce_begin(&vs->ring);
        int q_timeout = frame_ring_quiesce_wait(&vs->ring, 1000) != 0;
        if (q_timeout)
            LOG_ERROR("video_stream: ring quiesce timeout (claims not drained)");

        close(epfd);
        deinit_video_device(vs);
        /* 超时兜底：STREAMOFF/munmap 后 claim 持有者只剩推理 worker（读的是
           job rgb 而非 mmap），通常随即写槽摘除 claim——再等一次；仍不归零
           （推理线程卡死）则强制清空槽，迟到写槽/摘除走防御分支无副作用 */
        if (q_timeout && frame_ring_quiesce_wait(&vs->ring, 1000) != 0) {
            LOG_ERROR("video_stream: quiesce retry timeout, force clearing slots");
            frame_ring_quiesce_force_clear(&vs->ring);
        }
        __atomic_store_n(&vs->restart_req, 0, __ATOMIC_RELEASE);
    }

    LOG_INFO("video_stream: worker exiting");
out:
    if (app->running) app->fatal = 1;
    app->running = 0;
    watchdog_unregister_self("video");
    return NULL;
}

/* out_capture_ms：该帧的采集时刻（CLOCK_MONOTONIC 毫秒，来自 V4L2 buf.timestamp），
   随帧发出 X-Ts 供前端算端到端延迟；无有效时间戳时为 0 */
static int video_stream_wait_next(int last_seq, unsigned char **out, size_t *out_len,
                                  unsigned long long *out_capture_ms)
{
    video_ctx_t *vs = g_ctx;
    *out_capture_ms = 0;
    if (!vs) return -1;
    unsigned long long last = (unsigned long long)(unsigned int)last_seq;

    /* 阻塞等新帧（环条件变量，帧到即醒；200ms 超时用于退出检查） */
    while (vs->app->running && vs->running) {
        if (frame_ring_wait_new(&vs->ring, last, 200)) break;
    }
    if (!vs->app->running || !vs->running) return -1;

    /* 持锁拷贝最新槽 raw：防止 worker 在复制期间释放该槽 */
    frame_ring_lock(&vs->ring);
    frame_slot_t *s = frame_ring_raw_newest_locked(&vs->ring);
    unsigned long long seq = s ? s->seq : last;
    if (s) {
        *out_capture_ms = s->capture_ms;
        unsigned char *copy = malloc(s->raw.len);
        if (!copy) {
            LOG_ERROR("video_stream: frame alloc failed (%zu bytes)", s->raw.len);
            *out     = NULL;
            *out_len = 0;
        } else {
            memcpy(copy, s->raw.buf, s->raw.len);
            *out     = copy;
            *out_len = s->raw.len;
        }
    }
    frame_ring_unlock(&vs->ring);
    return (int)(unsigned int)seq;   /* 客户端 pacing 兼容 32 位序号 */
}

/* 环形队列句柄：YOLO/录像消费方直接操作槽位（零拷贝路径），模块未初始化返回 NULL */
frame_ring_t *video_stream_get_ring(void)
{
    video_ctx_t *vs = g_ctx;
    return vs ? &vs->ring : NULL;
}

/* 运行时重启视频流（切换设备/分辨率，不重启进程） */
void video_stream_restart(void)
{
    video_ctx_t *vs = g_ctx;
    if (!vs) return;

    /* 更新参数（worker 加锁读取，避免与设备初始化并发） */
    pthread_mutex_lock(&vs->cfg_mutex);
    if (vs->app && vs->app->cfg) {
        if (vs->app->cfg->video_device[0])
            snprintf(vs->device, sizeof(vs->device), "%s", vs->app->cfg->video_device);
        if (vs->app->cfg->video_width > 0)
            vs->width = vs->app->cfg->video_width > 4096 ? 4096 : vs->app->cfg->video_width;
        if (vs->app->cfg->video_height > 0)
            vs->height = vs->app->cfg->video_height > 4096 ? 4096 : vs->app->cfg->video_height;
    }
    pthread_mutex_unlock(&vs->cfg_mutex);

    /* 请求 worker 用新参数重新初始化；旧设备由 worker 自行 deinit，
       避免 restart 与 worker 并发操作同一 fd 造成双重释放/崩溃 */
    __atomic_store_n(&vs->restart_req, 1, __ATOMIC_RELEASE);
    LOG_INFO("video_stream: restart requested (%s %dx%d)",
             vs->device, vs->width, vs->height);
}

/* Camera MJPEG passthrough: no image decoding or re-encoding. */
/* 完整写入非阻塞推流 socket 统一走 core/common.h 的 fd_write_all_blocking（处理
   EAGAIN/EWOULDBLOCK 与部分写入，避免慢客户端导致 MJPEG 流数据错位） */

typedef struct {
    int fd;
    video_stream_client_close_cb on_close;
} video_client_ctx_t;

static void *video_stream_client_task(void *arg)
{
    video_client_ctx_t *a = (video_client_ctx_t *)arg;
    int fd = a->fd;
    video_stream_client_close_cb on_close = a->on_close;
    free(a);

    video_ctx_t *vs = g_ctx;
    if (!vs) {
        if (on_close) on_close(fd);
        return NULL;
    }
    const char *hdr = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: multipart/x-mixed-replace; boundary=--frame\r\n"
                      "Connection: close\r\n\r\n";
    fd_write_all_blocking(fd, hdr, strlen(hdr));
    int last_raw = 0;   /* 原始帧 pacing：seq 单调递增，wait_next 依赖此不变量 */
    time_t last_write = time(NULL);   /* 最近一次成功写帧的时间，用于空闲超时 */

    /* 码率预算：1280x720 的相机原生 JPEG 约 60~110 KB/帧，30 fps 合 18~26 Mbps。
       无线链路给不到这么多时，socket 会持续阻塞、帧堆在发送缓冲里，表现为
       帧率骤降且延迟越拖越长。这里按每秒预算主动跳帧：画质不变（发的仍是
       相机原生帧，不重编码），只把帧率降到链路真正扛得住的水平。

       预算本身自适应：写入总是瞬间返回说明链路还有余量（加），写入大量阻塞
       说明已经饱和（减）。这样有线/5G/拥挤的 2.4G 都能收敛到各自的真实带宽，
       不用手工调参。mjpeg_max_kbps 只作为起始值和上限（0 = 完全不限）。 */
#define MJ_MIN_BPS  (3LL * 1000 * 1000 / 8)      /* 不低于 3 Mbps */
#define MJ_MAX_BPS  (40LL * 1000 * 1000 / 8)     /* 不高于 40 Mbps */
    long long budget_bytes = 0;
    if (vs->app && vs->app->cfg && vs->app->cfg->mjpeg_max_kbps > 0)
        budget_bytes = (long long)vs->app->cfg->mjpeg_max_kbps * 1000LL / 8LL;
    long long win_bytes = 0, slow_ms = 0;
    struct timespec win_t0;
    clock_gettime(CLOCK_MONOTONIC, &win_t0);

    while (vs->running && vs->app->running) {
        /* 空闲超时：长时间未成功写出任何帧（客户端不读/网络黑洞），释放连接，
           避免推流线程与 fd 永久占用 */
        if (time(NULL) - last_write >= VIDEO_CLIENT_IDLE_TIMEOUT) break;

        /* 对端断开检测：非阻塞 MSG_PEEK，FIN 已到（返回 0）或连接异常即退出 */
        char peek;
        ssize_t pr = recv(fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT);
        if (pr == 0) break;
        if (pr < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;

        /* 帧发布驱动推流；socket 写入有界等待，无需独立看门狗监督。 */
        unsigned char *frame = NULL; size_t flen = 0;
        unsigned long long capture_ms = 0;
        int seq = video_stream_wait_next(last_raw, &frame, &flen, &capture_ms);
        if (seq < 0) break;
        if (seq == last_raw) { usleep(20000); continue; }
        last_raw = seq;
        if (!frame || flen == 0) { usleep(20000); continue; }   /* 分配失败等：跳过 */

        /* 每秒窗口：先结算上一窗口的拥塞情况并调整预算，再判断本帧是否放行 */
        if (budget_bytes > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long el_ms = (long long)(now.tv_sec - win_t0.tv_sec) * 1000LL
                            + (long long)(now.tv_nsec - win_t0.tv_nsec) / 1000000LL;
            if (el_ms >= 1000) {
                /* 写阻塞占比高 = 链路饱和，下调；几乎不阻塞 = 还有余量，上调。
                   中间留 5%~20% 的死区，避免在临界点来回抖。 */
                if (slow_ms * 5 > el_ms)       budget_bytes = budget_bytes * 8 / 10;
                else if (slow_ms * 20 < el_ms) budget_bytes = budget_bytes * 11 / 10;
                if (budget_bytes < MJ_MIN_BPS) budget_bytes = MJ_MIN_BPS;
                if (budget_bytes > MJ_MAX_BPS) budget_bytes = MJ_MAX_BPS;
                win_bytes = 0; slow_ms = 0; win_t0 = now;
            }
            if (win_bytes + (long long)flen > budget_bytes) {
                free(frame);
                usleep(4000);
                continue;   /* last_raw 已前进，这一帧不再重发 */
            }
        }
        char mhdr[200];
        /* X-Seq：该帧在采集环里的序号（与 YOLO 结果 seq 同一空间）。前端据此把
           检测框叠加到对应原始帧，做到透传链路也能显示标注且不重新编码。
           X-Ts：该帧的采集时刻（板端单调时钟毫秒），前端配合 /api/video/stats 的
           now_ms 折算钟差后得到真实的采集→显示端到端延迟。 */
        int hlen = snprintf(mhdr, sizeof(mhdr),
                            "--frame\r\nContent-Type: image/jpeg\r\n"
                            "X-Seq: %u\r\nX-Ts: %llu\r\nContent-Length: %zu\r\n\r\n",
                            (unsigned)seq, capture_ms, flen);
        struct timespec w0, w1;
        clock_gettime(CLOCK_MONOTONIC, &w0);
        if (fd_write_all_blocking(fd, mhdr, (size_t)hlen) < 0) { free(frame); break; }
        if (fd_write_all_blocking(fd, frame, flen) < 0) { free(frame); break; }
        if (fd_write_all_blocking(fd, "\r\n", 2) < 0) { free(frame); break; }
        clock_gettime(CLOCK_MONOTONIC, &w1);
        if (budget_bytes > 0) {
            win_bytes += (long long)flen;
            slow_ms   += (long long)(w1.tv_sec - w0.tv_sec) * 1000LL
                       + (long long)(w1.tv_nsec - w0.tv_nsec) / 1000000LL;
        }
        free(frame);
        last_write = time(NULL);
    }
    if (on_close) on_close(fd);
    return NULL;
}

int video_stream_client_start(int fd, video_stream_client_close_cb on_close)
{
    video_client_ctx_t *a = malloc(sizeof(*a));
    if (!a) return -1;
    a->fd       = fd;
    a->on_close = on_close;
    if (http_worker_start(video_stream_client_task, a) != 0) {
        free(a);
        return -1;
    }
    return 0;
}
