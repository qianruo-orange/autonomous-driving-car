/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * agent_view.c — 智能体的视觉通道实现。
 *
 * 三个约束决定了这里的写法：
 *   1) 观测必须是无副作用的。所有数据都在锁内拷贝快照后立刻出锁，绝不把
 *      NPU 结果队列或帧环的锁带进 socket 写路径。
 *   2) 图像必须是相机原始字节，不做重编码。相机输出 MJPEG，重编码只会引入
 *      一次无意义的解码/编码往返，还会污染推理侧的时延统计。
 *   3) 响应体大小必须可预期。检测框默认最多 12 条、JSON 上限 8 KB；JPEG 有
 *      max_bytes 上限，超限直接 413，而不是把 4 MB 塞给上游模型。
 */
#define _GNU_SOURCE
#include "agent/agent_view.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "agent/agent_hw.h"
#include "control/image_tracking.h"
#include "core/log.h"
#include "http/http_internal.h"
#include "video/frame_ring.h"
#include "video/video_stream.h"
#include "yolo/rknn_yolo.h"

#define VIEW_MAX_OBJECTS     12
#define VIEW_HIST_RECORDS    8
#define VIEW_MAX_FRAME_BYTES (2u * 1024u * 1024u)

static int jadd(char *buf, size_t size, int off, const char *fmt, ...)
{
    if (off < 0 || (size_t)off >= size) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, size - (size_t)off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size - (size_t)off) return -1;
    return off + n;
}

/* 归一化到 0..1，异常输入返回 0，避免 NaN 进入 JSON。 */
static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static const char *range_class(double area_ratio)
{
    if (area_ratio >= 0.12) return "near";
    if (area_ratio >= 0.03) return "mid";
    return "far";
}

/* 类别名来自可上传的 names 文件，落地前过滤到 0x20..0x7e 并去引号。 */
static void class_name(const char *src, char *out, size_t size)
{
    size_t o = 0;
    for (; src && *src && o + 1 < size; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\' || c < 0x20 || c > 0x7e) continue;
        out[o++] = (char)c;
    }
    out[o] = 0;
    if (!o) snprintf(out, size, "object");
}

int agent_perception_json(char *out, size_t size, int off)
{
    if (!out || !size) return -1;

    yolo_result_t recs[VIEW_HIST_RECORDS];
    int n = rknn_yolo_results_since(0, recs, VIEW_HIST_RECORDS);
    const yolo_result_t *r = n > 0 ? &recs[n - 1] : NULL;

    frame_ring_t *ring = video_stream_get_ring();
    frame_ring_stats_t fs;
    memset(&fs, 0, sizeof(fs));
    if (ring) frame_ring_stats(ring, &fs);

    image_tracking_snapshot_t track;
    memset(&track, 0, sizeof(track));
    image_tracking_snapshot(&track);

    /* 相机在跑但检测落后于采集，说明推理侧掉队：这个差值本身就是给智能体的
     * 安全信号，不能只报“有没有检测到”。 */
    long long lag = (r && fs.produce_seq > r->seq) ? (long long)(fs.produce_seq - r->seq) : 0;
    unsigned long long age_ms = 0;
    if (r && r->post_done_us) {
        unsigned long long now_us = monotonic_ms() * 1000ull;
        if (now_us > r->post_done_us) age_ms = (now_us - r->post_done_us) / 1000ull;
    }

    off = jadd(out, size, off, "{\"available\":%s", r ? "true" : "false");
    if (off < 0) return -1;
    if (r) {
        off = jadd(out, size, off,
                   ",\"frame_seq\":%llu,\"width\":%d,\"height\":%d,\"age_ms\":%llu,"
                   "\"frames_behind\":%lld,\"infer_age_ms\":%llu",
                   r->seq, r->w, r->h, age_ms, lag, fs.processing_age_ms);
        if (off < 0) return -1;
    }

    /* 面积最大者即"最近的那个东西"，作为给语言模型的单值摘要。 */
    int nearest = -1;
    double nearest_area = 0;
    int shown = r ? (r->count > VIEW_MAX_OBJECTS ? VIEW_MAX_OBJECTS : r->count) : 0;
    for (int i = 0; i < shown; i++) {
        const yolo_det_t *d = &r->dets[i];
        double bw = (double)d->x2 - d->x1, bh = (double)d->y2 - d->y1;
        if (bw <= 0 || bh <= 0) continue;
        double area = (r->w > 0 && r->h > 0) ? (bw * bh) / ((double)r->w * r->h) : 0;
        if (area > nearest_area) {
            nearest_area = area;
            nearest = i;
        }
    }

    off = jadd(out, size, off, ",\"objects_total\":%d,\"objects\":[",
               r ? r->count : 0);
    if (off < 0) return -1;
    int emitted = 0;
    for (int i = 0; i < shown; i++) {
        const yolo_det_t *d = &r->dets[i];
        double bw = (double)d->x2 - d->x1, bh = (double)d->y2 - d->y1;
        if (bw <= 0 || bh <= 0 || r->w <= 0 || r->h <= 0) continue;
        double cx = clamp01(((double)d->x1 + bw / 2) / r->w);
        double cy = clamp01(((double)d->y1 + bh / 2) / r->h);
        double area = (bw * bh) / ((double)r->w * r->h);
        char name[24];
        class_name(rknn_yolo_class_name(d->cls), name, sizeof(name));
        off = jadd(out, size, off,
                   "%s{\"cls\":%d,\"name\":\"%s\",\"conf\":%.3f,"
                   "\"box\":[%d,%d,%d,%d],\"center\":[%.3f,%.3f],"
                   "\"lateral\":%.3f,\"area_ratio\":%.4f,\"range\":\"%s\","
                   "\"in_path\":%s}",
                   emitted ? "," : "", d->cls, name, (double)d->conf,
                   (int)d->x1, (int)d->y1, (int)d->x2, (int)d->y2,
                   cx, cy, cx - 0.5, area, range_class(area),
                   (cy > 0.55 && fabs(cx - 0.5) < 0.28) ? "true" : "false");
        if (off < 0) return -1;
        emitted++;
    }
    off = jadd(out, size, off, "]");
    if (off < 0) return -1;

    if (nearest >= 0 && r) {
        const yolo_det_t *d = &r->dets[nearest];
        double bw = (double)d->x2 - d->x1, bh = (double)d->y2 - d->y1;
        double cx = clamp01(((double)d->x1 + bw / 2) / r->w);
        double area = (bw * bh) / ((double)r->w * r->h);
        char name[24];
        class_name(rknn_yolo_class_name(d->cls), name, sizeof(name));
        off = jadd(out, size, off,
                   ",\"nearest\":{\"name\":\"%s\",\"conf\":%.3f,\"lateral\":%.3f,"
                   "\"area_ratio\":%.4f,\"range\":\"%s\"}",
                   name, (double)d->conf, cx - 0.5, area, range_class(area));
        if (off < 0) return -1;
    } else {
        off = jadd(out, size, off, ",\"nearest\":null");
        if (off < 0) return -1;
    }

    off = jadd(out, size, off,
               ",\"tracking\":{\"active\":%s,\"locked\":%s,\"track_id\":%llu,"
               "\"age_ms\":%llu}",
               track.active ? "true" : "false", track.selected ? "true" : "false",
               track.track_id, track.age_ms);
    if (off < 0) return -1;

    /* 一句话摘要：让"看到"这件事不必依赖调用方自己再算一遍几何。 */
    char summary[160];
    const char *side = "centre";
    double lateral = 0;
    if (nearest >= 0 && r) {
        const yolo_det_t *d = &r->dets[nearest];
        lateral = clamp01((((double)d->x1 + ((double)d->x2 - d->x1) / 2) / r->w)) - 0.5;
    }
    if (lateral < -0.12) side = "left";
    else if (lateral > 0.12) side = "right";
    if (!r) {
        snprintf(summary, sizeof(summary), "no detections yet");
    } else if (nearest < 0) {
        snprintf(summary, sizeof(summary), "clear road, %d frame(s) behind", (int)lag);
    } else {
        char name[24];
        class_name(rknn_yolo_class_name(r->dets[nearest].cls), name, sizeof(name));
        snprintf(summary, sizeof(summary), "%s %s, %s range, conf %.2f",
                 name, side, range_class(nearest_area), (double)r->dets[nearest].conf);
    }
    char esc[200];
    http_json_escape(summary, esc, sizeof(esc));
    off = jadd(out, size, off, ",\"summary\":\"%s\"}", esc);
    return off < 0 ? -1 : off;
}

void http_agent_perception(app_ctx_t *app, int fd, const char *method,
                           const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri;
    if (agent_cap_authorize(req, AGENT_SCOPE_OBSERVE) < 0) {
        http_err(fd, 401, "Unauthorized", "missing or invalid X-Agent-Cap");
        return;
    }
    char json[8192];
    int off = jadd(json, sizeof(json), 0, "{\"ts_ms\":%llu,\"perception\":", monotonic_ms());
    if (off < 0) goto overflow;
    off = agent_perception_json(json, sizeof(json), off);
    if (off < 0) goto overflow;
    off = jadd(json, sizeof(json), off, "}");
    if (off < 0) goto overflow;
    http_ok_json(fd, json, (size_t)off);
    return;
overflow:
    http_err(fd, 500, "Error", "perception snapshot too large");
}

/* 相机帧：锁内只做一次 memcpy，锁外发送。 */
static int frame_copy(unsigned char **out, size_t *len, unsigned long long *seq,
                      int *w, int *h, unsigned long long *age_ms)
{
    frame_ring_t *ring = video_stream_get_ring();
    if (!ring) return -1;
    int rc = -1;
    *out = NULL;
    *len = 0;
    frame_ring_lock(ring);
    const frame_slot_t *best = NULL;
    for (int i = 0; i < FRAME_RING_N; i++) {
        const frame_slot_t *s = &ring->slots[i];
        if (!s->raw_present || !s->raw.buf || s->raw.len == 0) continue;
        if (s->fmt != FRAME_RING_FMT_MJPEG) continue;
        if (!best || s->seq > best->seq) best = s;
    }
    if (best) {
        unsigned char *copy = malloc(best->raw.len);
        if (copy) {
            memcpy(copy, best->raw.buf, best->raw.len);
            *out = copy;
            *len = best->raw.len;
            *seq = best->seq;
            *w = best->w;
            *h = best->h;
            unsigned long long now = monotonic_ms();
            *age_ms = best->deliver_ms && now > best->deliver_ms ? now - best->deliver_ms : 0;
            rc = 0;
        }
    }
    frame_ring_unlock(ring);
    return rc;
}

static const char b64_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const unsigned char *src, size_t len)
{
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)src[i] << 16;
        if (i + 1 < len) v |= (unsigned)src[i + 1] << 8;
        if (i + 2 < len) v |= src[i + 2];
        out[o++] = b64_tab[(v >> 18) & 63];
        out[o++] = b64_tab[(v >> 12) & 63];
        out[o++] = i + 1 < len ? b64_tab[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? b64_tab[v & 63] : '=';
    }
    out[o] = 0;
    return out;
}

void http_agent_frame(app_ctx_t *app, int fd, const char *method,
                      const char *uri, const char *req)
{
    (void)app; (void)method;
    if (agent_cap_authorize(req, AGENT_SCOPE_OBSERVE) < 0) {
        http_err(fd, 401, "Unauthorized", "missing or invalid X-Agent-Cap");
        return;
    }
    int b64 = 0;
    size_t max_bytes = 0; /* 0 = 使用默认上限 */
    const char *q = uri ? strchr(uri, '?') : NULL;
    if (q && strstr(q, "b64=1")) b64 = 1;
    if (q) {
        const char *m = strstr(q, "max_bytes=");
        if (m) {
            char *end = NULL;
            unsigned long v = strtoul(m + 10, &end, 10);
            if (end && end != m + 10 && v > 1024 && v <= VIEW_MAX_FRAME_BYTES) max_bytes = (size_t)v;
        }
    }

    unsigned char *jpeg = NULL;
    size_t len = 0;
    unsigned long long seq = 0, age = 0;
    int w = 0, h = 0;
    if (frame_copy(&jpeg, &len, &seq, &w, &h, &age) < 0) {
        http_err(fd, 503, "Service Unavailable",
                 "no MJPEG frame available (camera stopped, or YUYV pipeline)");
        return;
    }
    if (len > (max_bytes ? max_bytes : VIEW_MAX_FRAME_BYTES)) {
        free(jpeg);
        http_err(fd, 413, "Payload Too Large", "frame exceeds max_bytes");
        return;
    }

    if (b64) {
        char *enc = b64_encode(jpeg, len);
        free(jpeg);
        if (!enc) {
            http_err(fd, 500, "Error", "out of memory");
            return;
        }
        size_t cap = strlen(enc) + 320;
        char *json = malloc(cap);
        if (!json) {
            free(enc);
            http_err(fd, 500, "Error", "out of memory");
            return;
        }
        int off = snprintf(json, cap,
                           "{\"seq\":%llu,\"width\":%d,\"height\":%d,\"age_ms\":%llu,"
                           "\"mime\":\"image/jpeg\",\"bytes\":%zu,\"jpeg_b64\":\"",
                           seq, w, h, age, len);
        if (off > 0 && (size_t)off + strlen(enc) + 4 < cap) {
            memcpy(json + off, enc, strlen(enc));
            off += (int)strlen(enc);
            json[off++] = '"';
            json[off++] = '}';
            http_ok_json(fd, json, (size_t)off);
        } else {
            http_err(fd, 500, "Error", "response too large");
        }
        free(enc);
        free(json);
        return;
    }

    char header[320];
    int hn = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: image/jpeg\r\n"
                      "Content-Length: %zu\r\n"
                      "X-Frame-Seq: %llu\r\n"
                      "X-Frame-Width: %d\r\n"
                      "X-Frame-Height: %d\r\n"
                      "X-Frame-Age-Ms: %llu\r\n"
                      "Cache-Control: no-store\r\n"
                      "Connection: close\r\n\r\n",
                      len, seq, w, h, age);
    if (hn <= 0 || hn >= (int)sizeof(header)) {
        free(jpeg);
        http_err(fd, 500, "Error", "header overflow");
        return;
    }
    if (http_write_all(fd, header, (size_t)hn) != 0 ||
        http_write_all(fd, jpeg, len) != 0)
        LOG_WARN("agent: short write while serving a frame");
    free(jpeg);
}
