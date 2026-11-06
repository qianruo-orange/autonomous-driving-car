/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * video_session.c — 录像会话管理：把带框的 H.264 写进 MP4。
 *
 * 链路：V4L2 原始 JPEG → NV12 → 画框 → H.264 → MP4（详见 video/video_record.c）。
 *   本文件只负责"什么时候开一段、写到哪个文件、什么时候切段"，一帧的编解码
 *   全在 video_record 线程里完成。
 *
 * 线程模型：main 模块表 "rec" 线程，空闲时 100ms 轮询喂狗；
 *   默认自动录制：任务启动即开始，文件平铺在 recordings/videos/，
 *   一天一个 video_YYYYMMDD.mp4，跨过零点自动收尾并续录新一天的文件；
 *   HTTP 线程可下发 video_session_start/stop 手动控制，手动停止后不再自动续录。
 *
 * 录像不依赖 YOLO：没有检测框的帧同样入队（recordings 里只有一份 MP4，
 * 框按帧画在 Y 平面上）。
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>

#include "video/video_session.h"
#include "video/video_record.h"
#include "video/video_stream.h"
#include "video/frame_ring.h"
#include "core/common.h"
#include "core/log.h"
#include "watchdog/watchdog.h"

#define REC_DIR        PATH_VIDEOS

/* 低于此剩余空间就不再开新段，避免录到一半 ENOSPC 后反复建/删空文件 */
#define REC_MIN_FREE_BYTES   (512ull * 1024ull * 1024ull)
/* I/O 失败后的退避，避免磁盘满时空转刷日志 */
#define REC_IO_BACKOFF_US    (5 * 1000 * 1000)

/* 命令（HTTP 线程 → 录制线程） */
#define REC_CMD_NONE   0
#define REC_CMD_START  1
#define REC_CMD_STOP   2

/* 模块全局上下文 */
static struct {
    pthread_mutex_t lock;
    int  cmd;                     /* REC_CMD_*（HTTP 线程下发） */
    int  recording;               /* 录制中标志（线程写，start 前读） */
    int  start_fail;              /* 最近一次 start 失败原因（0=无） */
    char file[128];               /* 当前录制文件名 */
    char day[16];                 /* 当前文件对应的日期 YYYYMMDD，跨天收尾 */
    uint64_t start_ms, frames, bytes;
    uint64_t last_frame_ts;       /* 最近一帧单调时钟，用于实时 fps */
    int split_pending, split_error;
    char encoder[32];
    /* 本节会话累计的跳过帧与错误态（供 /api/rec/status） */
    unsigned long long skipped_cadence, skipped_size;
    int io_error;
    video_record_t *annot;        /* 标注录像（H.264/MP4），唯一的录像产物 */
} g_rec;

/* Serialize publication/finalization of file paths with file deletion. This
 * lock is never held during frame writes, network writes, or idle waits.
 * If both locks are needed, acquire files_lock before g_rec.lock. */
static pthread_mutex_t g_rec_files_lock = PTHREAD_MUTEX_INITIALIZER;

int video_session_delete_file(int directory_fd, const char *name)
{
    size_t length = name ? strlen(name) : 0;
    if (length < 10 || length >= 256 || strchr(name, '/') || strstr(name, "..") ||
        (strncmp(name, "video_", 6)) ||
        (strcmp(name + length - 4, ".mp4"))) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!isalnum(ch) && ch != '_' && ch != '-' && ch != '.') {
            errno = EINVAL;
            return -1;
        }
    }
    pthread_mutex_lock(&g_rec_files_lock);
    pthread_mutex_lock(&g_rec.lock);
    /* recording may already be zero while the file is still being finalized. */
    int active = g_rec.file[0] && !strcmp(g_rec.file, name);
    pthread_mutex_unlock(&g_rec.lock);
    int result = 1;
    if (!active) {
        struct stat st;
        if (fstatat(directory_fd, name, &st, AT_SYMLINK_NOFOLLOW))
            result = errno == ENOENT ? 2 : -1;
        else if (!S_ISREG(st.st_mode)) {
            errno = EINVAL;
            result = -1;
        } else if (unlinkat(directory_fd, name, 0))
            result = errno == ENOENT ? 2 : -1;
        else
            result = 0;
    }
    int error = errno;
    pthread_mutex_unlock(&g_rec_files_lock);
    errno = error;
    return result;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 录像按自然日分文件，文件名里的日期就是这里的 YYYYMMDD */
static void rec_today(char *out, size_t size)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(out, size, "%Y%m%d", &tm);
}

/* ---- 取帧：相机原始 JPEG ---- */

static unsigned long long g_raw_seq = 0;       /* 已读取原始帧序号（探测/测帧率用） */
static unsigned long long g_raw_rec_seq = 0;   /* 录像已消费到的原始帧序号 */

/* 取 seq 大于游标的最小可用 raw 槽，锁内整帧拷贝（调用方 free）。
   raw 指向 V4L2 mmap：采集侧 QBUF 后缓冲归驱动，只能在锁内读完，
   因此这里是拷贝而非零拷贝窃取。录像线程由 CAPTURED 事件唤醒，
   正常节奏下拷贝发生在帧仍被钉住的窗口内；确实落后时跳过中间帧
   并在 *gap 里返回跳过的帧数，而不是把旧帧重复写进文件。 */
static int rec_take_raw_jpeg(frame_ring_t *ring, unsigned char **out, size_t *len,
                             int *w, int *h, unsigned long long *seq_out,
                             unsigned long long *gap)
{
    if (!ring) return -1;
    frame_ring_lock(ring);
    frame_slot_t *best = NULL;
    for (int i = 0; i < FRAME_RING_N; i++) {
        frame_slot_t *s = &ring->slots[i];
        if (!s->raw_present || !s->raw.buf || s->raw.len == 0) continue;
        if (s->fmt != FRAME_RING_FMT_MJPEG) continue;
        if (s->seq <= g_raw_rec_seq) continue;
        if (!best || s->seq < best->seq) best = s;
    }
    if (!best) { frame_ring_unlock(ring); return -1; }
    unsigned char *copy = malloc(best->raw.len);
    if (!copy) { frame_ring_unlock(ring); return -1; }
    memcpy(copy, best->raw.buf, best->raw.len);
    if (gap) *gap = best->seq - g_raw_rec_seq - 1;
    g_raw_rec_seq = best->seq;
    *out = copy;
    *len = best->raw.len;
    if (w) *w = best->w;
    if (h) *h = best->h;
    if (seq_out) *seq_out = best->seq;
    frame_ring_unlock(ring);
    return 0;
}

/* 读取最新原始帧（锁内拷贝，*out 为 malloc，调用方 free）。
   仅供录制会话开始前的分辨率探测与帧率实测使用。 */
static int rec_take_raw_frame(frame_ring_t *ring, unsigned char **out, size_t *len,
                              int *fmt, int *w, int *h)
{
    if (!ring) return -1;
    frame_ring_lock(ring);
    frame_slot_t *s = frame_ring_raw_newest_locked(ring);
    if (!s || s->seq == g_raw_seq) { frame_ring_unlock(ring); return -1; }
    unsigned char *raw = malloc(s->raw.len ? s->raw.len : 1);
    if (!raw) { frame_ring_unlock(ring); return -1; }
    memcpy(raw, s->raw.buf, s->raw.len);
    unsigned long long seq = s->seq;
    size_t raw_len = s->raw.len;
    int raw_fmt = s->fmt, raw_w = s->w, raw_h = s->h;
    frame_ring_unlock(ring);
    g_raw_seq = seq;
    *out = raw;
    *len = raw_len;
    if (fmt) *fmt = raw_fmt;
    if (w) *w = raw_w;
    if (h) *h = raw_h;
    return 0;
}

/* 实测帧率（不写死）：连续抓若干新帧取平均间隔，fps = 1000/avg_ms。
   填 MP4 的 mvhd/tkhd 时间基；失败返回 0（无视频帧）。 */
static int rec_measure_fps(app_ctx_t *app, frame_ring_t *ring)
{
    enum { N = 8 };
    uint64_t ts[N];
    int n = 0;
    uint64_t last = 0;
    uint64_t t0 = now_ms();
    while (n < N && app->running) {
        watchdog_feed_self("rec");
        unsigned char *f = NULL; size_t flen = 0;
        int fmt = 0;
        if (rec_take_raw_frame(ring, &f, &flen, &fmt, NULL, NULL) == 0) {
            uint64_t t = now_ms();
            if (t != last) { ts[n++] = t; last = t; }
            free(f);
        } else {
            usleep(20000);
        }
        if (now_ms() - t0 > 2000) break;   /* 2s 量测超时 */
    }
    if (n < 3) return 0;
    uint64_t span = ts[n - 1] - ts[0];
    uint64_t avg = span / (n - 1);
    if (avg == 0) avg = 1;
    int fps = (int)(1000 / avg);
    if (fps < 1) fps = 1;
    if (fps > 120) fps = 120;
    return fps;
}

/* ---- 剩余空间、帧时间旁路、孤儿段修复 ---- */

static int rec_free_bytes(unsigned long long *out)
{
    struct statvfs vfs;
    if (statvfs(REC_DIR, &vfs) != 0) return -1;
    *out = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
    return 0;
}

/* ---- 录制线程 ---- */

void *video_session_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    (void)pthread_setname_np(pthread_self(), "rec");
    if (watchdog_register_thread(pthread_self(), "rec", 5, 3) < 0) {
        app->fatal = 1;
        app->running = 0;
        return NULL;
    }
    frame_ring_t *ring = video_stream_get_ring();
    int auto_rec = 1;                    /* 默认自动开启录制 */

    while (app->running) {
        watchdog_feed_self("rec");

        /* 取命令（无命令则空转喂狗） */
        pthread_mutex_lock(&g_rec.lock);
        int cmd = g_rec.cmd;
        g_rec.cmd = REC_CMD_NONE;
        pthread_mutex_unlock(&g_rec.lock);

        if (cmd == REC_CMD_START) {
            auto_rec = 1;
        } else if (cmd == REC_CMD_STOP) {
            pthread_mutex_lock(&g_rec.lock);
            g_rec.recording = 0;
            pthread_mutex_unlock(&g_rec.lock);
            auto_rec = 0;                /* 手动停止后不再自动续录 */
        }

        if (auto_rec && !g_rec.recording) {
            /* ---- 开始一个录制会话（自动开启/续录或命令启动） ---- */
            int pw = app->cfg->video_width, ph = app->cfg->video_height;
            if (pw <= 0 || ph <= 0) {    /* 配置未设分辨率：首帧探测 */
                unsigned char *probe = NULL; size_t plen = 0;
                int fmt = 0;
                if (rec_take_raw_frame(ring, &probe, &plen, &fmt, &pw, &ph) != 0 ||
                    pw <= 0 || ph <= 0) {
                    if (probe) free(probe);
                    pthread_mutex_lock(&g_rec.lock);
                    g_rec.start_fail = 1;   /* 无视频帧且无配置分辨率 */
                    pthread_mutex_unlock(&g_rec.lock);
                    usleep(100000);
                    continue;
                }
                free(probe);
            }

            int fps = rec_measure_fps(app, ring);
            if (fps <= 0) {
                pthread_mutex_lock(&g_rec.lock);
                g_rec.start_fail = 1;   /* 无视频帧 */
                pthread_mutex_unlock(&g_rec.lock);
                usleep(100000);
                continue;
            }

            /* 所有录像平铺在 recordings/videos/，日期只在文件名里 */
            char dir[512];
            snprintf(dir, sizeof(dir), "%s", REC_DIR);

            char fname[128] = "";

            /* 空间不足时不要开段：否则写入立刻 ENOSPC，只会反复建/删空文件 */
            unsigned long long free_bytes = 0;
            if (rec_free_bytes(&free_bytes) != 0 || free_bytes < REC_MIN_FREE_BYTES) {
                pthread_mutex_lock(&g_rec.lock);
                g_rec.start_fail = 4;
                g_rec.io_error = 3;
                pthread_mutex_unlock(&g_rec.lock);
                LOG_ERROR("rec: only %llu MiB free (need %llu MiB), not starting",
                          free_bytes >> 20, (unsigned long long)REC_MIN_FREE_BYTES >> 20);
                usleep(REC_IO_BACKOFF_US);
                continue;
            }

            pthread_mutex_lock(&g_rec_files_lock);
            /* MP4 是唯一产物：解码、画框、编码、封装都在 video_record 线程里 */
            video_record_stop(g_rec.annot);
            g_rec.annot = video_record_start(dir, "video", pw, ph, fps, 0,
                                          fname, sizeof(fname));
            if (!g_rec.annot) {
                pthread_mutex_unlock(&g_rec_files_lock);
                pthread_mutex_lock(&g_rec.lock);
                g_rec.start_fail = 3;
                g_rec.io_error = 1;
                pthread_mutex_unlock(&g_rec.lock);
                LOG_ERROR("rec: create recording file failed");
                usleep(100000);
                continue;
            }
            /* 从采集环当前游标起步，别把会话开始前的旧帧写进新文件 */
            if (ring) {
                frame_ring_lock(ring);
                g_raw_rec_seq = ring->produce_seq;
                frame_ring_unlock(ring);
            }
            char rel[192];
            snprintf(rel, sizeof(rel), "%s", fname);
            LOG_INFO("rec: recording started -> %s (%dx%d @%dfps, H.264/MP4, FIXQP)",
                     rel, pw, ph, fps);
            pthread_mutex_lock(&g_rec.lock);
            g_rec.recording = 1;
            safe_strncpy(g_rec.encoder, sizeof(g_rec.encoder), "h264-mp4");
            g_rec.start_fail = 0;
            g_rec.start_ms = now_ms();
            g_rec.frames = 0;
            g_rec.bytes = 0;
            g_rec.last_frame_ts = 0;
            g_rec.split_pending = 0;
            g_rec.split_error = 0;
            g_rec.skipped_cadence = 0;
            g_rec.skipped_size = 0;
            g_rec.io_error = 0;
            safe_strncpy(g_rec.file, sizeof(g_rec.file), rel);
            rec_today(g_rec.day, sizeof(g_rec.day));
            pthread_mutex_unlock(&g_rec.lock);
            pthread_mutex_unlock(&g_rec_files_lock);

            /* ---- 录制内层循环 ---- */
            int done = 0;
            unsigned long long rec_raw_gap = 0;   /* 因落后于采集而跳过的帧 */
            unsigned long long rec_size_skip = 0; /* 尺寸与配置不符而跳过的帧 */
            int io_backoff = 0;                   /* 本段因 I/O 失败收尾，需要退避 */
            while (app->running && !done) {
                watchdog_feed_self("rec");
                pthread_mutex_lock(&g_rec.lock);
                int still = g_rec.recording;
                pthread_mutex_unlock(&g_rec.lock);
                if (!still) break;

                unsigned char *jpg = NULL;
                size_t jlen = 0;
                int jw = 0, jh = 0;
                unsigned long long jseq = 0, gap = 0;
                if (rec_take_raw_jpeg(ring, &jpg, &jlen, &jw, &jh, &jseq, &gap) != 0) {
                    /* 落后时 CAPTURED 已翻页：按 seq 等待，避免空转 */
                    if (ring) frame_ring_wait_new(ring, g_raw_rec_seq, 50);
                    else usleep(10000);
                    continue;
                }
                rec_raw_gap += gap;
                if (gap) {
                    pthread_mutex_lock(&g_rec.lock);
                    g_rec.skipped_cadence = rec_raw_gap;
                    pthread_mutex_unlock(&g_rec.lock);
                }
                if (jw != pw || jh != ph) {
                    free(jpg);
                    rec_size_skip++;
                    pthread_mutex_lock(&g_rec.lock);
                    g_rec.skipped_size = rec_size_skip;
                    pthread_mutex_unlock(&g_rec.lock);
                    continue;
                }

                /* 每天一个文件：跨过零点就收尾当前段，下一段用新日期命名 */
                {
                    char today[16];
                    rec_today(today, sizeof(today));
                    pthread_mutex_lock(&g_rec.lock);
                    if (strcmp(today, g_rec.day)) g_rec.split_pending = 1;
                    pthread_mutex_unlock(&g_rec.lock);
                }

                pthread_mutex_lock(&g_rec.lock);
                int split = g_rec.split_pending && g_rec.recording;
                pthread_mutex_unlock(&g_rec.lock);
                if (split) {
                    /* 收尾当前文件（写入 moov）后立刻续录下一个：新编码器第一帧
                       就是 IDR，两段之间只有一次编码器重建的间隔 */
                    char next_name[128] = "", next_dir[512];
                    snprintf(next_dir, sizeof(next_dir), "%s", REC_DIR);
                    pthread_mutex_lock(&g_rec_files_lock);
                    video_record_stop(g_rec.annot);
                    g_rec.annot = video_record_start(next_dir, "video", pw, ph, fps, 0,
                                                  next_name, sizeof(next_name));
                    if (g_rec.annot) {
                        int finalized = 0;
                        snprintf(rel, sizeof(rel), "%s", next_name);
                        pthread_mutex_lock(&g_rec.lock);
                        safe_strncpy(g_rec.file, sizeof(g_rec.file), rel);
                        rec_today(g_rec.day, sizeof(g_rec.day));
                        g_rec.start_ms = now_ms();
                        g_rec.frames = g_rec.bytes = 0;
                        g_rec.last_frame_ts = 0;
                        g_rec.split_pending = 0;
                        g_rec.split_error = finalized == 0 ? 0 : 2;
                        pthread_mutex_unlock(&g_rec.lock);
                        LOG_INFO("rec: continuing in new file -> %s", rel);
                    } else {
                        pthread_mutex_lock(&g_rec.lock);
                        g_rec.split_pending = 0;
                        g_rec.split_error = 1;
                        pthread_mutex_unlock(&g_rec.lock);
                        LOG_ERROR("rec: split file creation failed; continuing current recording");
                    }
                    pthread_mutex_unlock(&g_rec_files_lock);
                }

                uint64_t wts = now_ms();
                free(jpg);
                pthread_mutex_lock(&g_rec.lock);
                g_rec.frames = video_record_frames(g_rec.annot);
                g_rec.bytes  = video_record_bytes(g_rec.annot);
                g_rec.last_frame_ts = wts;
                pthread_mutex_unlock(&g_rec.lock);

            }

            /* ---- 结束会话（finalize 内部释放对象） ---- */
            uint32_t n_frames = (uint32_t)video_record_frames(g_rec.annot);
            uint32_t n_bytes  = (uint32_t)video_record_bytes(g_rec.annot);
            pthread_mutex_lock(&g_rec_files_lock);
            video_record_stop(g_rec.annot);
            g_rec.annot = NULL;
            pthread_mutex_lock(&g_rec.lock);
            g_rec.recording = 0;
            g_rec.split_pending = 0;
            g_rec.start_fail = 0;
            g_rec.file[0] = '\0';
            pthread_mutex_unlock(&g_rec.lock);
            pthread_mutex_unlock(&g_rec_files_lock);
            if (rec_raw_gap || rec_size_skip)
                LOG_INFO("rec: skipped %llu frames (cadence), %llu (size)",
                         rec_raw_gap, rec_size_skip);
            if (n_frames > 0)
                LOG_INFO("rec: recording finished -> %s (%u frames, %u bytes)",
                         rel, n_frames, n_bytes);
            else
                LOG_INFO("rec: recording aborted (no frames): %s", rel);

            if (io_backoff) usleep(REC_IO_BACKOFF_US);   /* 磁盘满/IO 错误：退避，勿空转 */
            if (n_frames > 0 && auto_rec) continue;  /* 达上限：立即续录下一段 */
        }

        usleep(100000);   /* 空转：响应命令与喂狗 */
    }
    if (app->running) app->fatal = 1;
    app->running = 0;
    watchdog_unregister_self("rec");
    return NULL;
}

/* ---- 对外 API（HTTP Reactor 线程调用） ---- */

int video_session_start(void)
{
    pthread_mutex_lock(&g_rec.lock);
    if (g_rec.recording) {
        pthread_mutex_unlock(&g_rec.lock);
        return -1;   /* 已在录制 */
    }
    g_rec.cmd = REC_CMD_START;
    g_rec.start_fail = 0;
    pthread_mutex_unlock(&g_rec.lock);
    return 0;
}

int video_session_stop(void)
{
    pthread_mutex_lock(&g_rec.lock);
    if (!g_rec.recording) {
        pthread_mutex_unlock(&g_rec.lock);
        return -1;
    }
    g_rec.cmd = REC_CMD_STOP;
    /* The inner loop polls recording, while cmd is consumed only by the outer
     * loop. Release the current session now so it can finalize; the queued
     * STOP then disables automatic continuation before the next file. */
    g_rec.recording = 0;
    pthread_mutex_unlock(&g_rec.lock);
    return 0;
}

int video_session_split(const char *expected_file)
{
    pthread_mutex_lock(&g_rec.lock);
    int result = 0;
    if (!g_rec.recording || !g_rec.file[0] || !g_rec.frames || g_rec.split_pending ||
        g_rec.cmd == REC_CMD_STOP)
        result = -1;
    else if (!expected_file || strcmp(expected_file, g_rec.file) != 0)
        result = -2;
    else {
        g_rec.split_pending = 1;
        g_rec.split_error = 0;
    }
    pthread_mutex_unlock(&g_rec.lock);
    return result;
}

void video_session_cycle(void)
{
    /* 直接清 recording 而不下发 REC_CMD_STOP：STOP 会同时关掉 auto_rec（视为
       用户手动停录），而这里只想让当前段收尾——录制线程随即按新配置续录下一段。
       手动停录状态下 recording 本就为 0，此处为空操作，不会擅自恢复录制 */
    pthread_mutex_lock(&g_rec.lock);
    g_rec.recording = 0;
    pthread_mutex_unlock(&g_rec.lock);
}

int video_session_status(video_session_status_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_rec.lock);
    out->recording = g_rec.recording;
    out->start_ms  = g_rec.start_ms;
    out->frames    = g_rec.frames;
    out->bytes     = g_rec.bytes;
    out->split_pending = g_rec.split_pending;
    out->split_error = g_rec.split_error;
    out->skipped_cadence = g_rec.skipped_cadence;
    out->skipped_size = g_rec.skipped_size;
    out->io_error = g_rec.io_error;
    safe_strncpy(out->encoder, sizeof(out->encoder),
                 g_rec.recording && g_rec.encoder[0] ? g_rec.encoder : "none");
    safe_strncpy(out->file, sizeof(out->file),
                 g_rec.file[0] ? g_rec.file : "");
    /* 实时帧率：最近一帧与前若干帧的间隔由调用方按 start_ms 粗估 */
    pthread_mutex_unlock(&g_rec.lock);
    if (rec_free_bytes(&out->free_bytes) != 0) out->free_bytes = 0;
    if (out->recording && out->frames > 1) {
        uint64_t el = now_ms() - out->start_ms;
        if (el > 0) out->fps = (double)out->frames * 1000.0 / (double)el;
    }
    return 0;
}

/* 逐级建目录：recordings/videos 的父目录可能还不存在 */
static void rec_mkdirs(const char *dir)
{
    char path[512];
    int length = snprintf(path, sizeof(path), "%s", dir);
    if (length <= 0 || (size_t)length >= sizeof(path)) return;
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        (void)mkdir(path, 0755);
        *p = '/';
    }
    (void)mkdir(path, 0755);
}

int video_session_init(void *arg)
{
    (void)arg;
    memset(&g_rec, 0, sizeof(g_rec));
    pthread_mutex_init(&g_rec.lock, NULL);
    rec_mkdirs(REC_DIR);
    LOG_INFO("rec: videos dir: %s", REC_DIR);
    return 0;
}

void video_session_destroy(void *arg)
{
    (void)arg;
    /* 停止录制并等待线程退出（录制循环 ≤1 帧间隔退出，finalize 同步完成） */
    pthread_mutex_lock(&g_rec.lock);
    g_rec.recording = 0;
    g_rec.cmd = REC_CMD_NONE;
    pthread_mutex_unlock(&g_rec.lock);
    pthread_mutex_destroy(&g_rec.lock);
}

void video_session_annot_frame(const void *nv12, int w, int h,
                           const video_record_box_t *boxes, int count)
{
    if (!nv12 || !boxes) return;
    if (count < 0) count = 0;
    if (count > 32) count = 32;
    if (g_rec.annot) video_record_push(g_rec.annot, nv12, w, h, boxes, count);
}
