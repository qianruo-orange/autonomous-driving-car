/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * frame_ring.c — 视频帧环形队列（详见 include/video/frame_ring.h）。
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "video/frame_ring.h"

/* 槽位索引：FRAME_RING_N 为 2 的幂，按位掩码取模 */
static frame_slot_t *slot_of(frame_ring_t *r, unsigned long long seq)
{
    return &r->slots[seq & (FRAME_RING_N - 1)];
}

static unsigned long long ring_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000u + (unsigned long long)ts.tv_nsec / 1000000u;
}

void frame_ring_init(frame_ring_t *r)
{
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->lock, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    for (int i = 0; i < FRAME_TOPIC_COUNT; i++)
        pthread_cond_init(&r->published[i], &attr);
    pthread_cond_init(&r->released, &attr);
    pthread_condattr_destroy(&attr);
}

void frame_ring_set_raw_release_cb(frame_ring_t *r,
                                   void (*cb)(void *arg, int vbuf_index),
                                   void *arg)
{
    r->raw_release_cb = cb;
    r->raw_release_arg = arg;
}

void frame_ring_lock(frame_ring_t *r)   { pthread_mutex_lock(&r->lock); }
void frame_ring_unlock(frame_ring_t *r) { pthread_mutex_unlock(&r->lock); }

static void cancel_waits_locked(frame_ring_t *r)
{
    r->cancel_generation++;
    for (int i = 0; i < FRAME_TOPIC_COUNT; i++)
        pthread_cond_broadcast(&r->published[i]);
}

void frame_ring_signal(frame_ring_t *r)
{
    frame_ring_lock(r);
    cancel_waits_locked(r);
    frame_ring_unlock(r);
}

/* ---- 释放与维护 ---- */

static void release_raw_locked(frame_ring_t *r, frame_slot_t *s)
{
    if (s->raw_is_mmap) {
        /* 延迟 QBUF：推理/编码消费完成前驱动缓冲一直钉在本槽（零拷贝关键） */
        if (r->raw_release_cb) r->raw_release_cb(r->raw_release_arg, s->raw_vbuf_index);
    } else {
        free(s->raw.buf);
    }
    s->raw.buf = NULL;
    s->raw.len = s->raw.cap = 0;
    s->raw_present = 0;
    s->raw_is_mmap = 0;
    s->raw_vbuf_index = -1;
}

/* 统一维护 raw 缓冲释放（遍历 32 槽，每帧若干次，开销可忽略）：
 * 非最新钉住（直播客户端锁内拷贝最新帧） + 无锁外读引用 +
 * 消费完成（YOLO 激活时等 infer_done） */
static void frame_ring_maintain_locked(frame_ring_t *r)
{
    for (int i = 0; i < FRAME_RING_N; i++) {
        frame_slot_t *s = &r->slots[i];
        if (!s->seq) continue;

        if (s->raw_present && s->seq != r->produce_seq && s->raw_claims == 0) {
            int consumed = r->yolo_active ? s->infer_done : 1;
            if (consumed) release_raw_locked(r, s);
        }
    }
}

/* ---- 采集 ---- */

frame_slot_t *frame_ring_produce_slot_locked(frame_ring_t *r)
{
    frame_ring_maintain_locked(r);
    frame_slot_t *s = slot_of(r, r->produce_seq + 1);
    /* raw 必须已释放且无 claim，否则本槽不可复用 → 丢帧 */
    if (s->raw_present || s->raw_claims)
        return NULL;
    memset(s, 0, sizeof(*s));
    s->seq = r->produce_seq + 1;
    s->raw_vbuf_index = -1;
    return s;
}

void frame_ring_produce_commit_locked(frame_ring_t *r)
{
    frame_slot_t *s = slot_of(r, r->produce_seq + 1);
    if (!s->capture_ms) s->capture_ms = ring_now_ms();
    r->produce_seq = s->seq;
    frame_ring_maintain_locked(r);
    pthread_cond_broadcast(&r->published[FRAME_TOPIC_CAPTURED]);
}

void frame_ring_produce_drop_locked(frame_ring_t *r)
{
    r->capture_dropped++;
}

void frame_ring_add_driver_dropped_locked(frame_ring_t *r, unsigned long long n)
{
    r->driver_dropped += n;
}

/* ---- 推理 ---- */

frame_slot_t *frame_ring_infer_claim_locked(frame_ring_t *r, unsigned long long seq)
{
    if (r->quiescing || seq == 0 || seq > r->produce_seq) return NULL;
    frame_slot_t *s = slot_of(r, seq);
    /* 槽可能已被释放（旧帧已被新帧复用）或采集已回收：跳过由调用方推进游标 */
    if (s->seq != seq || !s->raw_present) return NULL;
    s->raw_claims++;
    return s;
}

void frame_ring_infer_unclaim_locked(frame_ring_t *r, frame_slot_t *s, int decode_ok)
{
    if (!s) return;   /* 防御：claim 失败的调用方误传 NULL */
    if (s->raw_claims > 0) s->raw_claims--;
    s->infer_done = 1;                 /* 解码结束（含失败），raw 可释放 */
    if (!decode_ok) r->infer_dropped++;
    frame_ring_maintain_locked(r);
    if (r->quiescing) pthread_cond_broadcast(&r->released);
}

/* ---- 显示 ---- */

void frame_ring_display_commit_locked(frame_ring_t *r, frame_slot_t *s)
{
    r->displayed_frames++;
    if (s->capture_ms) {
        r->processing_age_ms = ring_now_ms() - s->capture_ms;
        if (r->processing_age_ms > r->processing_age_max_ms) {
            r->processing_age_max_ms = r->processing_age_ms;
            r->processing_age_max_at_ms = ring_now_ms();
        }
    }
    /* 最新结果优先：本帧与上次渲染之间被跳过的帧 */
    if (s->seq > r->display_seq + 1 && s->seq > r->display_seq)
        r->render_skipped += s->seq - r->display_seq - 1;
    r->display_seq = s->seq;
    frame_ring_maintain_locked(r);     /* 释放旧 jpeg */
}

/* ---- 原始帧探测（录像会话开始前） ---- */

frame_slot_t *frame_ring_raw_newest_locked(frame_ring_t *r)
{
    if (!r->produce_seq) return NULL;
    frame_slot_t *s = slot_of(r, r->produce_seq);
    if (s->seq != r->produce_seq || !s->raw_present) return NULL;
    return s;
}

/* ---- 模式标志 ---- */

void frame_ring_set_yolo_active(frame_ring_t *r, int on)
{
    frame_ring_lock(r);
    if (r->yolo_active != on) {
        r->yolo_active = on;
        frame_ring_maintain_locked(r);   /* 停用即放开 raw 释放条件 */
    }
    frame_ring_unlock(r);
}

/* ---- 事件等待 ---- */

static void timespec_add_ms(struct timespec *ts, int ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

void frame_ring_subscribe(frame_ring_t *r, frame_subscription_t *sub, frame_topic_t topic)
{
    frame_ring_lock(r);
    *sub = (frame_subscription_t){ r, topic, r->cancel_generation };
    frame_ring_unlock(r);
}

static int wait_for_frame(frame_subscription_t *sub, unsigned long long last_seq,
                          int timeout_ms)
{
    frame_ring_t *r = sub->ring;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    timespec_add_ms(&ts, timeout_ms);
    frame_ring_lock(r);
    while (sub->generation == r->cancel_generation) {
        int ready = sub->topic == FRAME_TOPIC_CAPTURED ? r->produce_seq > last_seq :
                                                         r->display_seq > last_seq;
        if (ready) {
            frame_ring_unlock(r);
            return 1;
        }
        if (pthread_cond_timedwait(&r->published[sub->topic], &r->lock, &ts) != 0) {
            frame_ring_unlock(r);
            return 0;
        }
    }
    sub->generation = r->cancel_generation;
    frame_ring_unlock(r);
    return 0;
}

int frame_ring_wait(frame_subscription_t *sub, unsigned long long last_seq, int timeout_ms)
{
    return wait_for_frame(sub, last_seq, timeout_ms);
}

int frame_ring_wait_new(frame_ring_t *r, unsigned long long last_seq, int timeout_ms)
{
    frame_subscription_t sub;
    frame_ring_subscribe(r, &sub, FRAME_TOPIC_CAPTURED);
    return frame_ring_wait(&sub, last_seq, timeout_ms);
}

/* ---- 相机重启退让 ---- */

void frame_ring_quiesce_begin(frame_ring_t *r)
{
    frame_ring_lock(r);
    r->quiescing = 1;
    cancel_waits_locked(r);
    frame_ring_unlock(r);
}

/* 清空槽：mmap 指针直接摘除（STREAMOFF 后驱动自收，不能 QBUF），
   非 mmap raw 释放。seq 保持单调不重置（须持锁） */
static void quiesce_clear_all_locked(frame_ring_t *r)
{
    for (int i = 0; i < FRAME_RING_N; i++) {
        frame_slot_t *s = &r->slots[i];
        if (s->raw_present && !s->raw_is_mmap) free(s->raw.buf);
        memset(s, 0, sizeof(*s));
    }
    r->quiescing = 0;
    cancel_waits_locked(r);
}

int frame_ring_quiesce_wait(frame_ring_t *r, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    timespec_add_ms(&ts, timeout_ms);
    frame_ring_lock(r);
    for (;;) {
        int claims = 0;
        for (int i = 0; i < FRAME_RING_N; i++) claims += r->slots[i].raw_claims;
        if (claims == 0) break;
        if (pthread_cond_timedwait(&r->released, &r->lock, &ts) != 0) {
            frame_ring_unlock(r);
            return -1;   /* 超时：调用方记 ERROR 后仍须执行 STREAMOFF/munmap */
        }
    }
    quiesce_clear_all_locked(r);
    frame_ring_unlock(r);
    return 0;
}

void frame_ring_quiesce_force_clear(frame_ring_t *r)
{
    frame_ring_lock(r);
    quiesce_clear_all_locked(r);
    frame_ring_unlock(r);
}

/* ---- 诊断 ---- */

void frame_ring_stats(frame_ring_t *r, frame_ring_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    frame_ring_lock(r);
    out->produce_seq      = r->produce_seq;
    out->display_seq      = r->display_seq;
    out->displayed_frames = r->displayed_frames;
    out->processing_age_ms = r->processing_age_ms;
    out->processing_age_max_ms = r->processing_age_max_ms;
    out->processing_age_max_at_ms = r->processing_age_max_at_ms;
    out->capture_fps = r->capture_fps_den ? (double)r->capture_fps_num / r->capture_fps_den : 0;
    out->capture_dropped  = r->capture_dropped;
    out->driver_dropped   = r->driver_dropped;
    out->capture_stall_ms = r->capture_stall_ms;
    out->infer_dropped    = r->infer_dropped;
    out->render_skipped   = r->render_skipped;
    for (int i = 0; i < FRAME_RING_N; i++)
        if (r->slots[i].raw_present) out->raw_pinned++;
    frame_ring_unlock(r);
}
