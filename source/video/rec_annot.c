/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * rec_annot.c —— 标注录像：NV12 画框 + MPP 硬件 H.264 + MP4 封装。
 *
 * 帧来自推理路径已经解好的 NV12 dma-buf；入队时拷一份（1.4 MB / 720p，约
 * 0.3 ms），之后 fd 可以立刻归还，避免和推理抢解码器缓冲。
 */
#define _GNU_SOURCE
#include "video/rec_annot.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#include <time.h>
#include <unistd.h>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_enc_cfg.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

#include "core/log.h"
#include "video/rec_mp4.h"

#define ANNOT_QUEUE_CAP  12
#define ANNOT_MAX_BOXES  32
#define ANNOT_SEGMENT_MB 512

typedef struct {
    unsigned char *nv12;
    int w, h;
    int count;
    rec_annot_box_t boxes[ANNOT_MAX_BOXES];
} annot_frame_t;

struct rec_annot {
    pthread_t tid;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int running;

    annot_frame_t q[ANNOT_QUEUE_CAP];
    int head, tail, count;

    char dir[256], prefix[64];
    char name[128];               /* 会话开始就定好文件名，MP4 按名创建 */
    int w, h, fps, bitrate_kbps;

    rec_mp4_t *mp4;
    uint64_t segment_bytes;

    MppCtx enc_ctx;
    MppApi *enc_mpi;
    MppBufferGroup grp;

    MppBuffer buf[4];

    int buf_next;    int enc_ready;

    atomic_ullong frames, dropped, bytes_total;
    atomic_uint last_ms;
    atomic_int   disabled;        /* 硬件不可用时置位，永不重试 */
};

static void draw_box_nv12(unsigned char *y, int w, int h,
                          const rec_annot_box_t *b, int thickness)
{
    int x1 = (int)(b->x1 + 0.5f), y1 = (int)(b->y1 + 0.5f);
    int x2 = (int)(b->x2 + 0.5f), y2 = (int)(b->y2 + 0.5f);
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > w - 1) x2 = w - 1;
    if (y2 > h - 1) y2 = h - 1;
    if (x2 - x1 < 2 || y2 - y1 < 2) return;
    for (int t = 0; t < thickness; t++) {
        int yt = y1 + t, yb = y2 - t;
        if (yt < h && yb >= 0) {
            memset(y + (size_t)yt * w + x1, 0xFF, (size_t)(x2 - x1 + 1));
            memset(y + (size_t)yb * w + x1, 0xFF, (size_t)(x2 - x1 + 1));
        }
        int xl = x1 + t, xr = x2 - t;
        if (xl >= 0 && xr < w) {
            for (int yy = y1; yy <= y2; yy++)
                if (yy >= 0 && yy < h) {
                    y[(size_t)yy * w + xl] = 0xFF;
                    y[(size_t)yy * w + xr] = 0xFF;
                }
        }
    }
}

static int encoder_open(rec_annot_t *s)
{
    if (mpp_create(&s->enc_ctx, &s->enc_mpi) != MPP_OK) return -1;
    if (mpp_init(s->enc_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) return -1;

    MppEncCfg cfg = NULL;
    if (mpp_enc_cfg_init(&cfg) != MPP_OK) return -1;
    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "prep:width", s->w);
    mpp_enc_cfg_set_s32(cfg, "prep:height", s->h);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", s->w);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", s->h);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "prep:range", MPP_FRAME_RANGE_JPEG);
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_VBR);
    mpp_enc_cfg_set_u32(cfg, "rc:max_reenc_times", 0);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", s->bitrate_kbps * 1000);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", s->bitrate_kbps * 1000 * 17 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", s->bitrate_kbps * 1000 * 15 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", s->fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", s->fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", s->fps * 2);
    /* 质量下限与上限：VBR 掉码率时不让 QP 无限放大 */
    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 24);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 10);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 32);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 10);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 30);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
    mpp_enc_cfg_set_s32(cfg, "h264:level", 40);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    if (s->enc_mpi->control(s->enc_ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
        mpp_enc_cfg_deinit(cfg);
        return -1;
    }
    mpp_enc_cfg_deinit(cfg);
    if (mpp_buffer_group_get_internal(&s->grp, MPP_BUFFER_TYPE_DRM) != MPP_OK) return -1;
    for (int i = 0; i < 4; i++)
        if (mpp_buffer_get(s->grp, &s->buf[i], (size_t)s->w * s->h * 3 / 2) != MPP_OK)
            return -1;
    s->enc_ready = 1;
    return 0;
}

/* MPP 可能把 SPS/PPS/SEI 单独成包：缓存到下一个切片包，拼成完整访问单元。 */
static unsigned char *pending;
static size_t pending_len, pending_cap;

static int pkt_has_vcl(const unsigned char *buf, size_t len)
{
    for (size_t i = 0; i + 4 <= len; i++)
        if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1) {
            int t = buf[i+3] & 0x1f;
            if (t == 1 || t == 5) return 1;
            i += 2;
        }
    return 0;
}

static void annot_feed(rec_annot_t *s, const unsigned char *p, size_t len)
{
    if (!pkt_has_vcl(p, len)) {
        if (pending_len + len > pending_cap) {
            size_t cap = pending_len + len + 4096;
            unsigned char *grown = realloc(pending, cap);
            if (!grown) return;
            pending = grown; pending_cap = cap;
        }
        memcpy(pending + pending_len, p, len);
        pending_len += len;
        return;
    }
    if (pending_len) {
        unsigned char *joined = malloc(pending_len + len);
        if (joined) {
            memcpy(joined, pending, pending_len);
            memcpy(joined + pending_len, p, len);
            rec_mp4_write(s->mp4, joined, pending_len + len);
            free(joined);
        } else {
            rec_mp4_write(s->mp4, p, len);
            }
        pending_len = 0;
    } else {
        rec_mp4_write(s->mp4, p, len);
    }
}

static void drain_packets(rec_annot_t *s)
{
    for (;;) {
        MppPacket pkt = NULL;
        if (s->enc_mpi->encode_get_packet(s->enc_ctx, &pkt) != MPP_OK || !pkt) break;
        const unsigned char *ptr = mpp_packet_get_pos(pkt);
        size_t len = mpp_packet_get_length(pkt);
        int eos = mpp_packet_get_eos(pkt);
        if (len && s->mp4) annot_feed(s, ptr, len);
        if (len) { s->segment_bytes += len; atomic_fetch_add(&s->bytes_total, len); }
        mpp_packet_deinit(&pkt);
        if (eos) break;
    }
}

static int encode_frame(rec_annot_t *s, annot_frame_t *f)
{
    if (!s->mp4) {
        s->mp4 = rec_mp4_create_named(s->dir, s->name, s->w, s->h, s->fps);
        if (!s->mp4) return -1;
    }
    if (!s->enc_ready && encoder_open(s) != 0) {
        if (s->enc_ctx) { mpp_destroy(s->enc_ctx); s->enc_ctx = NULL; }
        if (s->grp) { mpp_buffer_group_put(s->grp); s->grp = NULL; }
        for (int i = 0; i < 4; i++)
            if (s->buf[i]) { mpp_buffer_put(s->buf[i]); s->buf[i] = NULL; }
        atomic_store(&s->disabled, 1);
        LOG_ERROR("rec_annot: 硬件编码器不可用，本次运行停用标注录像");
        return -1;
    }
    if (f->w != s->w || f->h != s->h) return -1;

    MppBuffer frame_buf = s->buf[s->buf_next];
    s->buf_next = (s->buf_next + 1) % 4;
    unsigned char *dst = mpp_buffer_get_ptr(frame_buf);
    memcpy(dst, f->nv12, (size_t)s->w * s->h * 3 / 2);
    for (int i = 0; i < f->count; i++) draw_box_nv12(dst, s->w, s->h, &f->boxes[i], 3);

    MppFrame out = NULL;
    if (mpp_frame_init(&out) != MPP_OK) return -1;
    mpp_frame_set_width(out, s->w);
    mpp_frame_set_height(out, s->h);
    mpp_frame_set_hor_stride(out, s->w);
    mpp_frame_set_ver_stride(out, s->h);
    mpp_frame_set_fmt(out, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(out, frame_buf);
    int rc = (s->enc_mpi->encode_put_frame(s->enc_ctx, out) == MPP_OK) ? 0 : -1;
    mpp_frame_deinit(&out);
    if (rc) return -1;
    drain_packets(s);
    if (s->segment_bytes >= (uint64_t)ANNOT_SEGMENT_MB * 1024 * 1024) {
        rec_mp4_close(s->mp4);
        s->mp4 = rec_mp4_create_named(s->dir, s->name, s->w, s->h, s->fps);
        s->segment_bytes = 0;
    }
    return 0;
}

static void *annot_task(void *arg)
{
    rec_annot_t *s = arg;
    pthread_setname_np(pthread_self(), "rec_annot");
    for (;;) {
        pthread_mutex_lock(&s->lock);
        while (s->running && s->count == 0) pthread_cond_wait(&s->wake, &s->lock);
        if (!s->running && s->count == 0) { pthread_mutex_unlock(&s->lock); break; }
        annot_frame_t f = s->q[s->head];
        s->head = (s->head + 1) % ANNOT_QUEUE_CAP;
        s->count--;
        pthread_mutex_unlock(&s->lock);

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (atomic_load(&s->disabled)) { free(f.nv12); break; }
        int ok = encode_frame(s, &f);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        unsigned ms = (unsigned)((t1.tv_sec - t0.tv_sec) * 1000 +
                                 (t1.tv_nsec - t0.tv_nsec) / 1000000);
        atomic_store(&s->last_ms, ms);
        atomic_fetch_add(ok == 0 ? &s->frames : &s->dropped, 1);
        free(f.nv12);
    }
    if (s->enc_ready) {
        MppFrame eos = NULL;
        if (mpp_frame_init(&eos) == MPP_OK) {
            mpp_frame_set_eos(eos, 1);
            s->enc_mpi->encode_put_frame(s->enc_ctx, eos);
            mpp_frame_deinit(&eos);
        }
        drain_packets(s);
        for (int i = 0; i < 4; i++)
            if (s->buf[i]) { mpp_buffer_put(s->buf[i]); s->buf[i] = NULL; }
        if (s->grp) { mpp_buffer_group_put(s->grp); s->grp = NULL; }
        mpp_destroy(s->enc_ctx);
    }
    if (s->mp4) { rec_mp4_close(s->mp4); s->mp4 = NULL; }
    LOG_INFO("rec_annot: 线程退出，已编码 %llu 帧",
             (unsigned long long)atomic_load(&s->frames));
    return NULL;
}

rec_annot_t *rec_annot_start(const char *dir, const char *prefix, int w, int h, int fps,
                             int bitrate_kbps, char *name_out, size_t name_size)
{
    if (!dir || !prefix || w <= 0 || h <= 0) return NULL;
    rec_annot_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    snprintf(s->dir, sizeof(s->dir), "%s", dir);
    snprintf(s->prefix, sizeof(s->prefix), "%s", prefix);
    {
        time_t now = time(NULL);
        struct tm tm;
        char when[32];
        localtime_r(&now, &tm);
        strftime(when, sizeof(when), "%Y%m%d_%H%M%S", &tm);
        snprintf(s->name, sizeof(s->name), "%s_%s.mp4", prefix, when);
        if (name_out && name_size) snprintf(name_out, name_size, "%s", s->name);
    }
    s->w = w; s->h = h;
    s->fps = fps > 0 ? fps : 30;
    /* 质量优先：720p 约 8 Mbps（约 0.29 bit/像素/帧），录出来的画面才干净 */
    s->bitrate_kbps = bitrate_kbps > 0 ? bitrate_kbps
                                       : (int)((long long)w * h * 30 * 45 / 100 / 1000 / 1000) * 1000;
    if (s->bitrate_kbps < 6000) s->bitrate_kbps = 6000;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->wake, NULL);
    s->running = 1;
    if (pthread_create(&s->tid, NULL, annot_task, s) != 0) {
        pthread_mutex_destroy(&s->lock);
        pthread_cond_destroy(&s->wake);
        free(s);
        return NULL;
    }
    LOG_INFO("rec_annot: 标注录像线程启动（%dx%d, %d kbps）", w, h, s->bitrate_kbps);
    return s;
}

int rec_annot_push(rec_annot_t *s, int nv12_fd, int w, int h,
                   const rec_annot_box_t *boxes, int count)
{
    if (!s || nv12_fd < 0 || w <= 0 || h <= 0) return -1;
    if (atomic_load((atomic_int *)&s->disabled)) return -1;
    size_t len = (size_t)w * h * 3 / 2;
    unsigned char *copy = malloc(len);
    if (!copy) return -1;
    void *src = mmap(NULL, len, PROT_READ, MAP_SHARED, nv12_fd, 0);
    if (src == MAP_FAILED) { free(copy); return -1; }
    /* 解码器由 DMA 写入，CPU 读取前必须让缓存失效，否则看到的是半旧数据 */
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    ioctl(nv12_fd, DMA_BUF_IOCTL_SYNC, &sync);
    memcpy(copy, src, len);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(nv12_fd, DMA_BUF_IOCTL_SYNC, &sync);
    munmap(src, len);

    pthread_mutex_lock(&s->lock);
    if (!s->running) { pthread_mutex_unlock(&s->lock); free(copy); return -1; }
    if (s->count == ANNOT_QUEUE_CAP) {
        annot_frame_t old = s->q[s->head];
        s->head = (s->head + 1) % ANNOT_QUEUE_CAP;
        s->count--;
        pthread_mutex_unlock(&s->lock);
        free(old.nv12);
        atomic_fetch_add(&s->dropped, 1);
        pthread_mutex_lock(&s->lock);
    }
    annot_frame_t *slot = &s->q[s->tail];
    slot->nv12 = copy;
    slot->w = w; slot->h = h;
    slot->count = count > ANNOT_MAX_BOXES ? ANNOT_MAX_BOXES : (count > 0 ? count : 0);
    for (int i = 0; i < slot->count; i++) slot->boxes[i] = boxes[i];
    s->tail = (s->tail + 1) % ANNOT_QUEUE_CAP;
    s->count++;
    pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

void rec_annot_stop(rec_annot_t *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->running = 0;
    pthread_cond_broadcast(&s->wake);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->tid, NULL);
    for (int i = 0; i < ANNOT_QUEUE_CAP; i++) free(s->q[i].nv12);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->wake);
    free(s);
}

void rec_annot_stats(const rec_annot_t *s, unsigned long long *frames,
                     unsigned long long *dropped, unsigned *last_ms)
{
    if (!s) return;
    if (frames) *frames = atomic_load(&s->frames);
    if (dropped) *dropped = atomic_load(&s->dropped);
    if (last_ms) *last_ms = atomic_load(&s->last_ms);
}

unsigned long long rec_annot_frames(const rec_annot_t *s)
{
    return s ? (unsigned long long)atomic_load(&s->frames) : 0;
}

unsigned long long rec_annot_bytes(const rec_annot_t *s)
{
    return s ? (unsigned long long)atomic_load(&s->bytes_total) : 0;
}

int rec_annot_active(const rec_annot_t *s)
{
    return s && s->mp4;
}
