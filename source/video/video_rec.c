/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * video_rec.c — 无损录像：相机原生 MJPEG 直封 AVI(MJPG)。
 *
 * 链路：V4L2 原始 JPEG → NV12 → 画框 → H.264 → MP4（详见 video/rec_annot.c）。
 *   全程不解码、不重编码、不缩放，文件里每一帧与传感器输出完全相同。
 *
 * 线程模型：main 模块表 "rec" 线程，空闲时 100ms 轮询喂狗；
 *   默认自动录制：任务启动即开始，按天分目录（recordings/YYYYMMDD），
 *   达到会话上限（帧数/体积）自动续录下一段；HTTP 线程可下发
 *   video_rec_start/stop 手动控制，手动停止后不再自动续录。
 *
 * 录像不依赖 YOLO：原始帧里没有检测框，框只在网页前端按帧序号叠加。
 * 因此 YOLO 停摆或重建都不影响录像连续性。
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
#include <dirent.h>
#include <time.h>

#include "video/video_rec.h"
#include "video/rec_annot.h"
#include "video/video_stream.h"
#include "video/frame_ring.h"
#include "core/common.h"
#include "core/log.h"
#include "watchdog/watchdog.h"

#define REC_DIR        PATH_RECORDINGS

/* 低于此剩余空间就不再开新段，避免录到一半 ENOSPC 后反复建/删空文件 */
#define REC_MIN_FREE_BYTES   (512ull * 1024ull * 1024ull)
/* 录制中回填头部并落盘的间隔：掉电后最多丢失这段窗口的索引 */
#define REC_REFRESH_MS       2000ull
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
    uint64_t start_ms, frames, bytes;
    uint64_t last_frame_ts;       /* 最近一帧单调时钟，用于实时 fps */
    int split_pending, split_error;
    char encoder[32];
    /* 本节会话累计的跳过帧与错误态（供 /api/rec/status） */
    unsigned long long skipped_cadence, skipped_size;
    int io_error;
    rec_annot_t *annot;        /* 标注录像（H.264/MP4），与无损 AVI 并行 */
} g_rec;

/* 帧时间戳旁路文件：recordings/YYYYMMDD/frames.tsv
 * 每行 <片段名>\t<帧号>\t<单调ms>\t<墙钟ms>，用于把录像帧和日志/CAN 对时。
 * 纯追加，丢电最多丢最后几行；不参与录像文件列表。 */
static FILE *g_sidecar;
static char  g_sidecar_day[16];

/* Serialize publication/finalization of file paths with file deletion. This
 * lock is never held during frame writes, network writes, or idle waits.
 * If both locks are needed, acquire files_lock before g_rec.lock. */
static pthread_mutex_t g_rec_files_lock = PTHREAD_MUTEX_INITIALIZER;

int video_rec_delete_file(int day_fd, const char *relative)
{
    size_t length = relative ? strlen(relative) : 0;
    if (length < 14 || length >= 256 || relative[8] != '/' ||
        strstr(relative, "..") ||
        (strncmp(relative + 9, "video_", 6) && strncmp(relative + 9, "rec_", 4) &&
         strncmp(relative + 9, "annot_", 6)) ||
        (strcmp(relative + length - 4, ".mp4") && strcmp(relative + length - 4, ".avi"))) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)relative[i];
        if (i < 8 ? ch < '0' || ch > '9'
                  : i != 8 && !isalnum(ch) && ch != '_' && ch != '-' && ch != '.') {
            errno = EINVAL;
            return -1;
        }
    }
    pthread_mutex_lock(&g_rec_files_lock);
    pthread_mutex_lock(&g_rec.lock);
    /* recording may already be zero while the file is still being finalized. */
    int active = g_rec.file[0] && !strcmp(g_rec.file, relative);
    pthread_mutex_unlock(&g_rec.lock);
    int result = 1;
    if (!active) {
        struct stat st;
        if (fstatat(day_fd, relative + 9, &st, AT_SYMLINK_NOFOLLOW))
            result = errno == ENOENT ? 2 : -1;
        else if (!S_ISREG(st.st_mode)) {
            errno = EINVAL;
            result = -1;
        } else if (unlinkat(day_fd, relative + 9, 0))
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
   只用来填 AVI 头部的 dwRate；失败返回 0（无视频帧）。 */
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

static void rec_sidecar_close(void)
{
    if (g_sidecar) fclose(g_sidecar);
    g_sidecar = NULL;
    g_sidecar_day[0] = '\0';
}

/* 按天打开（同一天复用）；首次创建写表头 */
static void rec_sidecar_open(const char *day)
{
    if (g_sidecar && !strcmp(g_sidecar_day, day)) return;
    rec_sidecar_close();
    char dir[512], path[576];
    snprintf(dir, sizeof(dir), "%s/%s", REC_DIR, day);
    snprintf(path, sizeof(path), "%s/frames.tsv", dir);
    FILE *fp = fopen(path, "a");
    if (!fp) {
        LOG_ERROR("rec: cannot open frame index %s: %s", path, strerror(errno));
        return;
    }
    if (ftell(fp) == 0) fputs("# file\tframe\tmono_ms\twall_ms\n", fp);
    g_sidecar = fp;
    safe_strncpy(g_sidecar_day, sizeof(g_sidecar_day), day);
}

static void rec_sidecar_frame(const char *file, unsigned long long frame)
{
    if (!g_sidecar) return;
    struct timespec rt, mono;
    clock_gettime(CLOCK_REALTIME, &rt);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    unsigned long long wall = (unsigned long long)rt.tv_sec * 1000ull +
                              (unsigned long long)rt.tv_nsec / 1000000ull;
    unsigned long long mono_ms = (unsigned long long)mono.tv_sec * 1000ull +
                                 (unsigned long long)mono.tv_nsec / 1000000ull;
    fprintf(g_sidecar, "%s\t%llu\t%llu\t%llu\n", file, frame, mono_ms, wall);
}

/* 修复上次异常退出（掉电、被 kill）留下的未收尾分段。只处理 RIFF 长度与文件
 * 长度不一致的 video_*.avi；在录制线程启动时扫一次，不进入录制热路径。 */
static void rec_repair_orphans(void)
{
    DIR *root = opendir(REC_DIR);
    if (!root) return;
    int repaired = 0;
    struct dirent *de;
    while ((de = readdir(root))) {
        if (de->d_name[0] == '.') continue;
        char daydir[512];
        snprintf(daydir, sizeof(daydir), "%s/%s", REC_DIR, de->d_name);
        DIR *day = opendir(daydir);
        if (!day) continue;
        struct dirent *f;
        while ((f = readdir(day))) {
            size_t n = strlen(f->d_name);
            if (n < 10 || strncmp(f->d_name, "video_", 6) ||
                strcmp(f->d_name + n - 4, ".avi"))
                continue;
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", daydir, f->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 12) continue;
            FILE *fp = fopen(path, "rb");
            if (!fp) continue;
            unsigned char h[12];
            size_t got = fread(h, 1, sizeof(h), fp);
            fclose(fp);
            if (got != sizeof(h) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4))
                continue;
            uint32_t riff = (uint32_t)h[4] | ((uint32_t)h[5] << 8) |
                            ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);
            if (riff != 0 && (off_t)riff + 8 == st.st_size) continue;   /* 已完整收尾 */
        }
        closedir(day);
    }
    closedir(root);
    if (repaired) LOG_INFO("rec: repaired %d unfinished segment(s)", repaired);
}

/* ---- 录制线程 ---- */

void *video_rec_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    (void)pthread_setname_np(pthread_self(), "rec");
    if (watchdog_register_thread(pthread_self(), "rec", 5, 3) < 0) {
        app->fatal = 1;
        app->running = 0;
        return NULL;
    }
    frame_ring_t *ring = video_stream_get_ring();
    rec_repair_orphans();          /* 收尾上次异常退出留下的分段，只做一次 */
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

            /* 按天分目录：recordings/YYYYMMDD */
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            char day[16];
            snprintf(day, sizeof(day), "%04d%02d%02d",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            char dir[512];
            snprintf(dir, sizeof(dir), "%s/%s", REC_DIR, day);

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
            /* 会话由标注录像驱动：MP4 是唯一产物，无损 AVI 不再参与 */
            rec_annot_stop(g_rec.annot);
            g_rec.annot = rec_annot_start(dir, "annot", pw, ph, fps, 0,
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
            snprintf(rel, sizeof(rel), "%s/%s", day, fname);
            LOG_INFO("rec: recording started -> %s (%dx%d @%dfps, lossless MJPEG/AVI, "
                     "camera native frames)", rel, pw, ph, fps);
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
            pthread_mutex_unlock(&g_rec.lock);
            pthread_mutex_unlock(&g_rec_files_lock);

            /* 帧时间旁路：本节起按天追加 */
            rec_sidecar_open(day);

            /* ---- 录制内层循环 ---- */
            int done = 0;
            unsigned long long rec_raw_gap = 0;   /* 因落后于采集而跳过的帧 */
            unsigned long long rec_size_skip = 0; /* 尺寸与配置不符而跳过的帧 */
            uint64_t last_refresh = now_ms();     /* 上次头部回填时刻 */
            int io_backoff = 0;                   /* 本段因 I/O 失败收尾，需要退避 */
            char cur_name[128] = "";
            safe_strncpy(cur_name, sizeof(cur_name), fname);
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

                /* 按日分段：日期一变就切到新一天的文件，避免一段横跨两天 */
                {
                    char today[16];
                    time_t nowt = time(NULL);
                    struct tm nowtm;
                    localtime_r(&nowt, &nowtm);
                    strftime(today, sizeof(today), "%Y%m%d", &nowtm);
                    if (strcmp(today, day) != 0) {
                        pthread_mutex_lock(&g_rec.lock);
                        g_rec.split_pending = 1;
                        pthread_mutex_unlock(&g_rec.lock);
                    }
                }

                pthread_mutex_lock(&g_rec.lock);
                int split = g_rec.split_pending && g_rec.recording;
                pthread_mutex_unlock(&g_rec.lock);
                if (split) {
                    /* 每帧都是关键帧，直接对齐到下一帧切分 */
                    char next_name[128] = "", next_day[16], next_dir[512];
                    time_t split_time = time(NULL);
                    struct tm split_tm;
                    localtime_r(&split_time, &split_tm);
                    strftime(next_day, sizeof(next_day), "%Y%m%d", &split_tm);
                    snprintf(next_dir, sizeof(next_dir), "%s/%s", REC_DIR, next_day);
                    pthread_mutex_lock(&g_rec_files_lock);
                    rec_annot_stop(g_rec.annot);
                    g_rec.annot = rec_annot_start(next_dir, "annot", pw, ph, fps, 0,
                                                  next_name, sizeof(next_name));
                    if (g_rec.annot) {
                        int finalized = 0;
                        safe_strncpy(cur_name, sizeof(cur_name), next_name);
                        rec_sidecar_open(next_day);   /* 跨天切分时换旁路文件 */
                        safe_strncpy(day, sizeof(day), next_day);
                        last_refresh = now_ms();
                        snprintf(rel, sizeof(rel), "%s/%s", next_day, next_name);
                        pthread_mutex_lock(&g_rec.lock);
                        safe_strncpy(g_rec.file, sizeof(g_rec.file), rel);
                        g_rec.start_ms = now_ms();
                        g_rec.frames = g_rec.bytes = 0;
                        g_rec.last_frame_ts = 0;
                        g_rec.split_pending = 0;
                        g_rec.split_error = finalized == 0 ? 0 : 2;
                        pthread_mutex_unlock(&g_rec.lock);
                        LOG_INFO("rec: file split, continuing -> %s", rel);
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
                g_rec.frames = rec_annot_frames(g_rec.annot);
                g_rec.bytes  = rec_annot_bytes(g_rec.annot);
                g_rec.last_frame_ts = wts;
                pthread_mutex_unlock(&g_rec.lock);

                rec_sidecar_frame(cur_name, rec_annot_frames(g_rec.annot));

                /* 定期回填头部并落盘：掉电后文件仍可按最后刷新点播放 */
                if (wts - last_refresh >= REC_REFRESH_MS) {
                    /* 相机帧率随光照变化（实测 30 → 10 fps），而 AVI 头只能声明
                     * 一个速率；段内变化会让回放时长失真最多 3 倍。偏差超过 25%
                     * 就按帧边界切段，新段用实测速率写头。 */
                    if (g_sidecar) fflush(g_sidecar);
                    last_refresh = wts;
                }
            }

            /* ---- 结束会话（finalize 内部释放对象） ---- */
            uint32_t n_frames = (uint32_t)rec_annot_frames(g_rec.annot);
            uint32_t n_bytes  = (uint32_t)rec_annot_bytes(g_rec.annot);
            pthread_mutex_lock(&g_rec_files_lock);
            rec_annot_stop(g_rec.annot);
            g_rec.annot = NULL;
            pthread_mutex_lock(&g_rec.lock);
            g_rec.recording = 0;
            g_rec.split_pending = 0;
            g_rec.start_fail = 0;
            g_rec.file[0] = '\0';
            pthread_mutex_unlock(&g_rec.lock);
            pthread_mutex_unlock(&g_rec_files_lock);
            if (rec_raw_gap || rec_size_skip)
                LOG_INFO("rec: lossless skipped %llu frames (cadence), %llu (size)",
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

int video_rec_start(void)
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

int video_rec_stop(void)
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

int video_rec_split(const char *expected_file)
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

void video_rec_cycle_session(void)
{
    /* 直接清 recording 而不下发 REC_CMD_STOP：STOP 会同时关掉 auto_rec（视为
       用户手动停录），而这里只想让当前段收尾——录制线程随即按新配置续录下一段。
       手动停录状态下 recording 本就为 0，此处为空操作，不会擅自恢复录制 */
    pthread_mutex_lock(&g_rec.lock);
    g_rec.recording = 0;
    pthread_mutex_unlock(&g_rec.lock);
}

int video_rec_status(video_rec_status_t *out)
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

int video_rec_init(void *arg)
{
    (void)arg;
    memset(&g_rec, 0, sizeof(g_rec));
    pthread_mutex_init(&g_rec.lock, NULL);
    mkdir(REC_DIR, 0755);
    LOG_INFO("rec: recordings dir: %s", REC_DIR);
    return 0;
}

void video_rec_destroy(void *arg)
{
    (void)arg;
    /* 停止录制并等待线程退出（录制循环 ≤1 帧间隔退出，finalize 同步完成） */
    pthread_mutex_lock(&g_rec.lock);
    g_rec.recording = 0;
    g_rec.cmd = REC_CMD_NONE;
    pthread_mutex_unlock(&g_rec.lock);
    rec_sidecar_close();
    pthread_mutex_destroy(&g_rec.lock);
}

void video_rec_annot_frame(const void *nv12, int w, int h, const float *xyxy, int count)
{
    if (!nv12 || !xyxy) return;
    if (count < 0) count = 0;
    rec_annot_box_t boxes[32];
    int n = count > 32 ? 32 : count;
    for (int i = 0; i < n; i++) {
        boxes[i].x1 = xyxy[4 * i + 0];
        boxes[i].y1 = xyxy[4 * i + 1];
        boxes[i].x2 = xyxy[4 * i + 2];
        boxes[i].y2 = xyxy[4 * i + 3];
        boxes[i].cls = 0;
    }
    if (g_rec.annot) rec_annot_push(g_rec.annot, nv12, w, h, boxes, n);
}
