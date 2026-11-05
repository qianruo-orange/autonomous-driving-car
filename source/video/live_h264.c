/* SPDX-License-Identifier: GPL-3.0-or-later */
/* live_h264.c —— 标注帧 → 硬件 H.264 → fMP4 分片 → 多客户端游标分发。 */
#define _GNU_SOURCE
#include "video/live_h264.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_enc_cfg.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>

#include "core/log.h"
#include "video/fmp4.h"

#define LIVE_QUEUE      8
#define LIVE_RING       128
#define LIVE_MAX_BOXES  32
#define LIVE_MAX_CLIENTS 8

typedef struct {
    unsigned char *nv12;
    int w, h, count;
    live_box_t boxes[LIVE_MAX_BOXES];
} live_frame_t;

typedef struct {
    unsigned char *data;
    size_t len;
    uint64_t seq;
    int used;
} live_slot_t;

typedef struct {
    int active;
    uint64_t seq;                 /* 下一个要读的分片序号 */
    unsigned char *buf;           /* 每客户端拷贝缓冲，保证发送期间指针有效 */
    size_t cap;
} live_client_t;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int running;
    pthread_t tid;

    live_frame_t q[LIVE_QUEUE];
    int head, tail, count;

    int w, h, fps, bitrate;

    MppCtx enc_ctx;
    MppApi *enc_mpi;
    MppBufferGroup grp;
    MppBuffer buf;
    int enc_ready;

    fmp4_t *muxer;
    live_slot_t ring[LIVE_RING];
    uint64_t write_seq, read_base;

    unsigned char *init;
    size_t init_len;

    live_client_t clients[LIVE_MAX_CLIENTS];

    atomic_ullong frames, dropped;
} L;

static void draw_box(unsigned char *y, int w, int h, const live_box_t *b, int t)
{
    int x1 = (int)b->x1, y1 = (int)b->y1, x2 = (int)b->x2, y2 = (int)b->y2;
    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 > w - 1) x2 = w - 1; if (y2 > h - 1) y2 = h - 1;
    if (x2 - x1 < 2 || y2 - y1 < 2) return;
    for (int k = 0; k < t; k++) {
        int yt = y1 + k, yb = y2 - k, xl = x1 + k, xr = x2 - k;
        if (yt < h) memset(y + (size_t)yt * w + x1, 0xFF, (size_t)(x2 - x1 + 1));
        if (yb >= 0 && yb < h) memset(y + (size_t)yb * w + x1, 0xFF, (size_t)(x2 - x1 + 1));
        if (xl >= 0 && xr < w)
            for (int yy = y1; yy <= y2; yy++)
                if (yy >= 0 && yy < h) { y[(size_t)yy * w + xl] = 0xFF; y[(size_t)yy * w + xr] = 0xFF; }
    }
}

static int encoder_open(void)
{
    if (mpp_create(&L.enc_ctx, &L.enc_mpi) != MPP_OK) return -1;
    if (mpp_init(L.enc_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) return -1;

    MppEncCfg cfg = NULL;
    if (mpp_enc_cfg_init(&cfg) != MPP_OK) return -1;
    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "prep:width", L.w);
    mpp_enc_cfg_set_s32(cfg, "prep:height", L.h);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", L.w);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", L.h);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "prep:range", MPP_FRAME_RANGE_JPEG);
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_VBR);
    mpp_enc_cfg_set_u32(cfg, "rc:max_reenc_times", 0);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", L.bitrate * 1000);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", L.bitrate * 1000 * 17 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", L.bitrate * 1000 * 15 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", L.fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", L.fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", L.fps * 2);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
    mpp_enc_cfg_set_s32(cfg, "h264:level", 40);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    if (L.enc_mpi->control(L.enc_ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
        mpp_enc_cfg_deinit(cfg);
        return -1;
    }
    mpp_enc_cfg_deinit(cfg);
    if (mpp_buffer_group_get_internal(&L.grp, MPP_BUFFER_TYPE_DRM) != MPP_OK) return -1;
    if (mpp_buffer_get(L.grp, &L.buf, (size_t)L.w * L.h * 3 / 2) != MPP_OK) return -1;
    L.enc_ready = 1;
    return 0;
}

/* 把 fmp4 产出的分片放进环形缓冲 */
static void publish_fragment(const unsigned char *data, size_t len)
{
    pthread_mutex_lock(&L.lock);
    int idx = (int)(L.write_seq % LIVE_RING);
    free(L.ring[idx].data);
    L.ring[idx].data = malloc(len);
    if (!L.ring[idx].data) {
        pthread_mutex_unlock(&L.lock);
        return;
    }
    memcpy(L.ring[idx].data, data, len);
    L.ring[idx].len = len;
    L.ring[idx].seq = L.write_seq;
    L.ring[idx].used = 1;
    if (!L.init && len > 8 && !memcmp(data + 4, "ftyp", 4)) {
        L.init = malloc(len);
        if (L.init) { memcpy(L.init, data, len); L.init_len = len; }
    }
    L.write_seq++;
    if (L.write_seq - L.read_base > LIVE_RING) L.read_base = L.write_seq - LIVE_RING;
    pthread_cond_broadcast(&L.wake);
    pthread_mutex_unlock(&L.lock);
}

static int encode_frame(live_frame_t *f)
{
    if (!L.muxer) {
        L.muxer = fmp4_create(L.w, L.h, L.fps);
        if (!L.muxer) return -1;
    }
    if (!L.enc_ready && encoder_open() != 0) {
        LOG_ERROR("live_h264: 硬件编码器初始化失败；直播 H.264 停用");
        return -1;
    }
    if (f->w != L.w || f->h != L.h) return -1;

    unsigned char *dst = mpp_buffer_get_ptr(L.buf);
    memcpy(dst, f->nv12, (size_t)L.w * L.h * 3 / 2);
    for (int i = 0; i < f->count; i++) draw_box(dst, L.w, L.h, &f->boxes[i], 3);

    MppFrame out = NULL;
    if (mpp_frame_init(&out) != MPP_OK) return -1;
    mpp_frame_set_width(out, L.w);
    mpp_frame_set_height(out, L.h);
    mpp_frame_set_hor_stride(out, L.w);
    mpp_frame_set_ver_stride(out, L.h);
    mpp_frame_set_fmt(out, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(out, L.buf);
    int rc = (L.enc_mpi->encode_put_frame(L.enc_ctx, out) == MPP_OK) ? 0 : -1;
    mpp_frame_deinit(&out);
    if (rc) return -1;

    for (;;) {
        MppPacket pkt = NULL;
        if (L.enc_mpi->encode_get_packet(L.enc_ctx, &pkt) != MPP_OK || !pkt) break;
        const unsigned char *p = mpp_packet_get_pos(pkt);
        size_t len = mpp_packet_get_length(pkt);
        if (len) fmp4_write(L.muxer, p, len);
        mpp_packet_deinit(&pkt);
    }
    const unsigned char *fb;
    size_t flen;
    while (fmp4_take(L.muxer, &fb, &flen)) publish_fragment(fb, flen);
    return 0;
}

static void *live_task(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "live_h264");
    for (;;) {
        pthread_mutex_lock(&L.lock);
        while (L.running && L.count == 0) pthread_cond_wait(&L.wake, &L.lock);
        if (!L.running && L.count == 0) { pthread_mutex_unlock(&L.lock); break; }
        live_frame_t f = L.q[L.head];
        L.head = (L.head + 1) % LIVE_QUEUE;
        L.count--;
        pthread_mutex_unlock(&L.lock);

        int ok = encode_frame(&f);
        atomic_fetch_add(ok == 0 ? &L.frames : &L.dropped, 1);
        free(f.nv12);
    }
    if (L.enc_ready) {
        if (L.buf) { mpp_buffer_put(L.buf); L.buf = NULL; }
        if (L.grp) { mpp_buffer_group_put(L.grp); L.grp = NULL; }
        mpp_destroy(L.enc_ctx);
        L.enc_ctx = NULL;
        L.enc_ready = 0;
    }
    if (L.muxer) { fmp4_destroy(L.muxer); L.muxer = NULL; }
    return NULL;
}

int live_h264_start(int w, int h, int fps, int bitrate_kbps)
{
    if (w <= 0 || h <= 0) return -1;
    memset(&L, 0, sizeof(L));
    pthread_mutex_init(&L.lock, NULL);
    pthread_cond_init(&L.wake, NULL);
    L.w = w; L.h = h;
    L.fps = fps > 0 ? fps : 30;
    L.bitrate = bitrate_kbps > 0 ? bitrate_kbps
                                 : (int)((long long)w * h * 30 / 8 / 1000 / 1000) * 1000;
    if (L.bitrate < 1000) L.bitrate = 1000;
    L.running = 1;
    if (pthread_create(&L.tid, NULL, live_task, NULL) != 0) {
        L.running = 0;
        return -1;
    }
    LOG_INFO("live_h264: 直播编码启动（%dx%d, %d kbps）", w, h, L.bitrate);
    return 0;
}

void live_h264_stop(void)
{
    if (!L.running) return;
    pthread_mutex_lock(&L.lock);
    L.running = 0;
    pthread_cond_broadcast(&L.wake);
    pthread_mutex_unlock(&L.lock);
    pthread_join(L.tid, NULL);
    for (int i = 0; i < LIVE_RING; i++) free(L.ring[i].data);
    for (int i = 0; i < LIVE_MAX_CLIENTS; i++) free(L.clients[i].buf);
    free(L.init);
    pthread_mutex_destroy(&L.lock);
    pthread_cond_destroy(&L.wake);
    memset(&L, 0, sizeof(L));
}

void live_h264_push(int nv12_fd, int w, int h, const live_box_t *boxes, int count)
{
    if (!L.running || nv12_fd < 0 || w <= 0 || h <= 0) return;
    size_t len = (size_t)w * h * 3 / 2;
    unsigned char *copy = malloc(len);
    if (!copy) return;
    void *src = mmap(NULL, len, PROT_READ, MAP_SHARED, nv12_fd, 0);
    if (src == MAP_FAILED) { free(copy); return; }
    memcpy(copy, src, len);
    munmap(src, len);

    pthread_mutex_lock(&L.lock);
    if (!L.running) { pthread_mutex_unlock(&L.lock); free(copy); return; }
    if (L.count == LIVE_QUEUE) {
        live_frame_t old = L.q[L.head];
        L.head = (L.head + 1) % LIVE_QUEUE;
        L.count--;
        pthread_mutex_unlock(&L.lock);
        free(old.nv12);
        atomic_fetch_add(&L.dropped, 1);
        pthread_mutex_lock(&L.lock);
    }
    live_frame_t *slot = &L.q[L.tail];
    slot->nv12 = copy;
    slot->w = w; slot->h = h;
    slot->count = count > LIVE_MAX_BOXES ? LIVE_MAX_BOXES : (count > 0 ? count : 0);
    for (int i = 0; i < slot->count; i++) slot->boxes[i] = boxes[i];
    L.tail = (L.tail + 1) % LIVE_QUEUE;
    L.count++;
    pthread_cond_signal(&L.wake);
    pthread_mutex_unlock(&L.lock);
}

int live_h264_attach(void)
{
    if (!L.running) return -1;
    pthread_mutex_lock(&L.lock);
    for (int i = 0; i < LIVE_MAX_CLIENTS; i++) {
        if (L.clients[i].active) continue;
        L.clients[i].active = 1;
        L.clients[i].seq = L.read_base;    /* 从最旧的保留分片开始 */
        pthread_mutex_unlock(&L.lock);
        return i;
    }
    pthread_mutex_unlock(&L.lock);
    return -1;
}

void live_h264_detach(int cursor)
{
    if (cursor < 0 || cursor >= LIVE_MAX_CLIENTS) return;
    pthread_mutex_lock(&L.lock);
    L.clients[cursor].active = 0;
    pthread_mutex_unlock(&L.lock);
}

int live_h264_next(int cursor, const unsigned char **buf, size_t *len)
{
    if (cursor < 0 || cursor >= LIVE_MAX_CLIENTS) return 0;
    int rc = 0;
    pthread_mutex_lock(&L.lock);
    live_client_t *c = &L.clients[cursor];
    if (!c->active) { pthread_mutex_unlock(&L.lock); return 0; }
    if (c->seq < L.read_base) {
        c->seq = L.read_base;             /* 落后：让它重新从 init 开始 */
        pthread_mutex_unlock(&L.lock);
        return -1;
    }
    if (c->seq >= L.write_seq) { pthread_mutex_unlock(&L.lock); return 0; }
    live_slot_t *slot = &L.ring[(int)(c->seq % LIVE_RING)];
    if (!slot->used || slot->seq != c->seq) { pthread_mutex_unlock(&L.lock); return 0; }
    if (c->cap < slot->len) {
        unsigned char *grown = realloc(c->buf, slot->len);
        if (!grown) { pthread_mutex_unlock(&L.lock); return 0; }
        c->buf = grown; c->cap = slot->len;
    }
    memcpy(c->buf, slot->data, slot->len);
    *buf = c->buf;
    *len = slot->len;
    c->seq++;
    rc = 1;
    pthread_mutex_unlock(&L.lock);
    return rc;
}

int live_h264_init(const unsigned char **buf, size_t *len)
{
    pthread_mutex_lock(&L.lock);
    if (!L.init) { pthread_mutex_unlock(&L.lock); return 0; }
    *buf = L.init;
    *len = L.init_len;
    pthread_mutex_unlock(&L.lock);
    return 1;
}
