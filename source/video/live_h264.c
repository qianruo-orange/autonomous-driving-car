/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * live_h264.c —— 直播分发。不自己编码：标注录像线程把同一份 H.264 访问单元
 * 交给这里，打成 fMP4 分片放进环形缓冲，每个 HTTP 客户端持游标取用。
 * 单编码器双出口，避免两个 MPP 实例争用同一颗 VEPU510。
 */
#define _GNU_SOURCE
#include "video/live_h264.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/log.h"
#include "video/fmp4.h"

#define LIVE_RING        128
#define LIVE_MAX_CLIENTS 8

typedef struct {
    unsigned char *data;
    size_t len;
    uint64_t seq;
    int used;
} live_slot_t;

typedef struct {
    int active;
    uint64_t seq;
    unsigned char *buf;
    size_t cap;
} live_client_t;

static struct {
    pthread_mutex_t lock;
    int running;
    int w, h, fps;
    fmp4_t *muxer;
    live_slot_t ring[LIVE_RING];
    uint64_t write_seq, read_base;
    unsigned char *init;
    size_t init_len;
    live_client_t clients[LIVE_MAX_CLIENTS];
    unsigned long long fragments;
} L;

/* 调用方必须已持有 L.lock */
static void publish_locked(const unsigned char *data, size_t len)
{
    int idx = (int)(L.write_seq % LIVE_RING);
    unsigned char *copy = malloc(len);
    if (!copy) return;
    memcpy(copy, data, len);
    free(L.ring[idx].data);
    L.ring[idx].data = copy;
    L.ring[idx].len = len;
    L.ring[idx].seq = L.write_seq;
    L.ring[idx].used = 1;
    if (!L.init && len > 8 && !memcmp(data + 4, "ftyp", 4)) {
        L.init = malloc(len);
        if (L.init) { memcpy(L.init, data, len); L.init_len = len; }
    }
    L.write_seq++;
    if (L.write_seq - L.read_base > LIVE_RING) L.read_base = L.write_seq - LIVE_RING;
    L.fragments++;
    if (L.fragments <= 3 || L.fragments % 300 == 0)
        LOG_INFO("live_h264: 分片 #%llu %zu 字节（init=%s）",
                 (unsigned long long)L.fragments, len, L.init ? "有" : "无");
}

int live_h264_start(int w, int h, int fps)
{
    if (w <= 0 || h <= 0) return -1;
    pthread_mutex_lock(&L.lock);
    if (L.running) { pthread_mutex_unlock(&L.lock); return 0; }
    L.muxer = fmp4_create(w, h, fps);
    if (!L.muxer) { pthread_mutex_unlock(&L.lock); return -1; }
    L.w = w; L.h = h; L.fps = fps > 0 ? fps : 30;
    L.running = 1;
    pthread_mutex_unlock(&L.lock);
    LOG_INFO("live_h264: 直播分发就绪（%dx%d）", w, h);
    return 0;
}

void live_h264_stop(void)
{
    pthread_mutex_lock(&L.lock);
    if (!L.running) { pthread_mutex_unlock(&L.lock); return; }
    L.running = 0;
    for (int i = 0; i < LIVE_RING; i++) { free(L.ring[i].data); L.ring[i].data = NULL; L.ring[i].used = 0; }
    for (int i = 0; i < LIVE_MAX_CLIENTS; i++) { free(L.clients[i].buf); L.clients[i].buf = NULL; L.clients[i].cap = 0; L.clients[i].active = 0; }
    free(L.init); L.init = NULL; L.init_len = 0;
    L.write_seq = L.read_base = 0;
    fmp4_destroy(L.muxer); L.muxer = NULL;
    pthread_mutex_unlock(&L.lock);
    LOG_INFO("live_h264: 直播分发停止");
}

void live_h264_publish(const unsigned char *annexb, size_t len)
{
    if (!annexb || len < 5) return;
    pthread_mutex_lock(&L.lock);
    if (!L.running || !L.muxer) { pthread_mutex_unlock(&L.lock); return; }
    fmp4_t *m = L.muxer;
    fmp4_write(m, annexb, len);
    const unsigned char *buf;
    size_t flen;
    /* take 返回的缓冲在下一次 take 前有效，这里立刻拷进环 */
    while (fmp4_take(m, &buf, &flen)) {
        publish_locked(buf, flen);
        free((void *)buf);
    }
    pthread_mutex_unlock(&L.lock);
}

int live_h264_attach(void)
{
    pthread_mutex_lock(&L.lock);
    if (!L.running) { pthread_mutex_unlock(&L.lock); return -1; }
    for (int i = 0; i < LIVE_MAX_CLIENTS; i++) {
        if (L.clients[i].active) continue;
        L.clients[i].active = 1;
        L.clients[i].seq = L.read_base;
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
    if (c->seq < L.read_base) { c->seq = L.read_base; pthread_mutex_unlock(&L.lock); return -1; }
    if (c->seq >= L.write_seq) { pthread_mutex_unlock(&L.lock); return 0; }
    live_slot_t *slot = &L.ring[(int)(c->seq % LIVE_RING)];
    if (!slot->used || slot->seq != c->seq) { pthread_mutex_unlock(&L.lock); return 0; }
    if (c->cap < slot->len) {
        unsigned char *grown = realloc(c->buf, slot->len);
        if (!grown) { pthread_mutex_unlock(&L.lock); return 0; }
        c->buf = grown; c->cap = slot->len;
    }
    memcpy(c->buf, slot->data, slot->len);
    *buf = c->buf; *len = slot->len;
    c->seq++;
    rc = 1;
    pthread_mutex_unlock(&L.lock);
    return rc;
}

int live_h264_init(const unsigned char **buf, size_t *len)
{
    pthread_mutex_lock(&L.lock);
    if (!L.init) { pthread_mutex_unlock(&L.lock); return 0; }
    *buf = L.init; *len = L.init_len;
    pthread_mutex_unlock(&L.lock);
    return 1;
}
