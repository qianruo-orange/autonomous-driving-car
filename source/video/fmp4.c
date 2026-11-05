/* SPDX-License-Identifier: GPL-3.0-or-later */
/* fmp4.c —— 直播用分片 MP4：init segment + 每帧一个 moof/mdat 分片。 */
#define _GNU_SOURCE
#include "video/fmp4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FMP4_QUEUE 64
#define FMP4_MAX_NAL 32

typedef struct { unsigned char *data; size_t len; } fmp4_buf_t;

struct fmp4 {
    int w, h, fps;
    unsigned char sps[512], pps[512];
    size_t sps_len, pps_len;
    uint32_t seq;
    uint32_t samples;
    fmp4_buf_t q[FMP4_QUEUE];
    int head, tail, count;
    unsigned char *au;
    size_t au_cap;
    int started;
};

/* ---- 写入缓冲 ---- */
typedef struct { unsigned char *p; size_t len, cap; } wb_t;

static int wb_need(wb_t *b, size_t n)
{
    if (b->len + n <= b->cap) return 0;
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->len + n) cap *= 2;
    unsigned char *p = realloc(b->p, cap);
    if (!p) return -1;
    b->p = p; b->cap = cap;
    return 0;
}

static void wb_u8(wb_t *b, unsigned v) { if (!wb_need(b, 1)) b->p[b->len++] = (unsigned char)v; }
static void wb_u16(wb_t *b, unsigned v) { wb_u8(b, v >> 8); wb_u8(b, v & 0xff); }
static void wb_u32(wb_t *b, uint32_t v)
{
    wb_u8(b, (unsigned)(v >> 24)); wb_u8(b, (unsigned)((v >> 16) & 0xff));
    wb_u8(b, (unsigned)((v >> 8) & 0xff)); wb_u8(b, (unsigned)(v & 0xff));
}
static void wb_u64(wb_t *b, uint64_t v) { wb_u32(b, (uint32_t)(v >> 32)); wb_u32(b, (uint32_t)v); }
static void wb_cc(wb_t *b, const char *s) { if (!wb_need(b, 4)) { memcpy(b->p + b->len, s, 4); b->len += 4; } }
static void wb_data(wb_t *b, const void *d, size_t n) { if (!wb_need(b, n)) { memcpy(b->p + b->len, d, n); b->len += n; } }
static void wb_zeros(wb_t *b, int n) { for (int i = 0; i < n; i++) wb_u8(b, 0); }

static size_t box_begin(wb_t *b, const char *type)
{
    size_t pos = b->len;
    wb_u32(b, 0);
    wb_cc(b, type);
    return pos;
}

static void box_end(wb_t *b, size_t pos)
{
    uint32_t size = (uint32_t)(b->len - pos);
    b->p[pos + 0] = (unsigned char)(size >> 24);
    b->p[pos + 1] = (unsigned char)(size >> 16);
    b->p[pos + 2] = (unsigned char)(size >> 8);
    b->p[pos + 3] = (unsigned char)size;
}

/* ---- NAL 遍历 ---- */
struct au_scan {
    unsigned char *out;      /* AVCC 缓冲 */
    size_t len, cap;
    int has_idr;
    unsigned char *sps, *pps;
    size_t *sps_len, *pps_len;
};

static void au_emit(void *ctx, const unsigned char *nal, size_t len)
{
    struct au_scan *a = ctx;
    if (!len) return;
    int type = nal[0] & 0x1f;
    if (type == 5) a->has_idr = 1;
    if (type == 7 && a->sps_len && !*a->sps_len && len <= 512) { memcpy(a->sps, nal, len); *a->sps_len = len; }
    if (type == 8 && a->pps_len && !*a->pps_len && len <= 512) { memcpy(a->pps, nal, len); *a->pps_len = len; }

    size_t need = a->len + 4 + len;
    if (need > a->cap) {
        size_t cap = a->cap ? a->cap * 2 : 4096;
        while (cap < need) cap *= 2;
        unsigned char *p = realloc(a->out, cap);
        if (!p) return;
        a->out = p; a->cap = cap;
    }
    a->out[a->len + 0] = (unsigned char)(len >> 24);
    a->out[a->len + 1] = (unsigned char)(len >> 16);
    a->out[a->len + 2] = (unsigned char)(len >> 8);
    a->out[a->len + 3] = (unsigned char)len;
    memcpy(a->out + a->len + 4, nal, len);
    a->len += 4 + len;
}

static void scan_nals(const unsigned char *buf, size_t len,
                      void (*cb)(void *, const unsigned char *, size_t), void *ctx)
{
    size_t i = 0;
    for (;;) {
        /* 找下一个起始码 */
        size_t payload = 0;
        int found = 0;
        for (size_t k = i; k + 3 <= len; k++) {
            if (buf[k] == 0 && buf[k+1] == 0 && buf[k+2] == 1) {
                payload = k + 3;
                found = 1;
                break;
            }
        }
        if (!found) break;
        /* 找这个 NAL 的结束位置；没有下一个起始码就一直到缓冲区末尾 */
        size_t next = len;
        for (size_t k = payload; k + 3 <= len; k++) {
            if (buf[k] == 0 && buf[k+1] == 0 && buf[k+2] == 1) { next = k; break; }
        }
        if (next > payload) cb(ctx, buf + payload, next - payload);
        i = next;
        if (next >= len) break;
    }
}

/* ---- init segment ---- */
static void write_init(fmp4_t *s, wb_t *b)
{
    /* ftyp */
    size_t f = box_begin(b, "ftyp");
    wb_cc(b, "isom"); wb_u32(b, 0x200);
    wb_cc(b, "isom"); wb_cc(b, "iso5"); wb_cc(b, "iso6"); wb_cc(b, "mp41");
    box_end(b, f);

    size_t moov = box_begin(b, "moov");

    /* mvhd：分片流用 duration=0 */
    size_t b1 = box_begin(b, "mvhd");
    wb_u32(b, 0); wb_u32(b, 0); wb_u32(b, 0);
    wb_u32(b, (uint32_t)s->fps); wb_u32(b, 0);
    wb_u32(b, 0x00010000); wb_u16(b, 0x0100); wb_zeros(b, 10);
    for (int i = 0; i < 9; i++) wb_u32(b, i == 0 || i == 4 || i == 8 ? 0x00010000 : 0);
    wb_zeros(b, 24);
    wb_u32(b, 2);
    box_end(b, b1);

    size_t trak = box_begin(b, "trak");

    /* tkhd */
    b1 = box_begin(b, "tkhd");
    wb_u32(b, 7); wb_u32(b, 0); wb_u32(b, 0); wb_u32(b, 1); wb_u32(b, 0); wb_u32(b, 0);
    wb_zeros(b, 8); wb_u16(b, 0); wb_u16(b, 0); wb_u16(b, 0); wb_u16(b, 0);
    for (int i = 0; i < 9; i++) wb_u32(b, i == 0 || i == 4 || i == 8 ? 0x00010000 : 0);
    wb_u32(b, (uint32_t)s->w << 16); wb_u32(b, (uint32_t)s->h << 16);
    box_end(b, b1);

    size_t mdia = box_begin(b, "mdia");
    b1 = box_begin(b, "mdhd");
    wb_u32(b, 0); wb_u32(b, 0); wb_u32(b, 0);
    wb_u32(b, (uint32_t)s->fps); wb_u32(b, 0);
    wb_u16(b, 0x55C4); wb_u16(b, 0);
    box_end(b, b1);

    b1 = box_begin(b, "hdlr");
    wb_u32(b, 0); wb_u32(b, 0); wb_cc(b, "vide"); wb_zeros(b, 12);
    wb_data(b, "VideoHandler", 12); wb_u8(b, 0);
    box_end(b, b1);

    size_t minf = box_begin(b, "minf");
    b1 = box_begin(b, "vmhd");
    wb_u32(b, 1); wb_u16(b, 0); wb_u16(b, 0); wb_u16(b, 0); wb_u16(b, 0);
    box_end(b, b1);

    b1 = box_begin(b, "dinf");
    size_t dref = box_begin(b, "dref");
    wb_u32(b, 0); wb_u32(b, 1);
    size_t url = box_begin(b, "url ");
    wb_u32(b, 1);
    box_end(b, url);
    box_end(b, dref);
    box_end(b, b1);

    size_t stbl = box_begin(b, "stbl");
    b1 = box_begin(b, "stsd");
    wb_u32(b, 0); wb_u32(b, 1);
    size_t avc1 = box_begin(b, "avc1");
    wb_zeros(b, 6); wb_u16(b, 1);
    wb_zeros(b, 16);
    wb_u16(b, (unsigned)s->w); wb_u16(b, (unsigned)s->h);
    wb_u32(b, 0x00480000); wb_u32(b, 0x00480000);
    wb_u32(b, 0); wb_u16(b, 1);
    wb_zeros(b, 32);
    wb_u16(b, 24); wb_u16(b, 0xFFFF);
    size_t avcC = box_begin(b, "avcC");
    wb_u8(b, 1);
    wb_u8(b, s->sps_len > 1 ? s->sps[1] : 0x64);
    wb_u8(b, s->sps_len > 2 ? s->sps[2] : 0);
    wb_u8(b, s->sps_len > 3 ? s->sps[3] : 0x28);
    wb_u8(b, 0xFF); wb_u8(b, 0xE1);
    wb_u16(b, (unsigned)s->sps_len); wb_data(b, s->sps, s->sps_len);
    wb_u8(b, 1); wb_u16(b, (unsigned)s->pps_len); wb_data(b, s->pps, s->pps_len);
    box_end(b, avcC);
    box_end(b, avc1);
    box_end(b, b1);

    /* 分片流：四个空表 */
    b1 = box_begin(b, "stts"); wb_u32(b, 0); wb_u32(b, 0); box_end(b, b1);
    b1 = box_begin(b, "stsc"); wb_u32(b, 0); wb_u32(b, 0); box_end(b, b1);
    b1 = box_begin(b, "stsz"); wb_u32(b, 0); wb_u32(b, 0); wb_u32(b, 0); box_end(b, b1);
    b1 = box_begin(b, "stco"); wb_u32(b, 0); wb_u32(b, 0); box_end(b, b1);

    box_end(b, stbl);
    box_end(b, minf);
    box_end(b, mdia);
    box_end(b, trak);

    /* mvex/trex：声明分片默认值 */
    size_t mvex = box_begin(b, "mvex");
    b1 = box_begin(b, "trex");
    wb_u32(b, 0); wb_u32(b, 1); wb_u32(b, 1); wb_u32(b, 1); wb_u32(b, 0); wb_u32(b, 0);
    box_end(b, b1);
    box_end(b, mvex);

    box_end(b, moov);
}

/* ---- 分片 ---- */
static void write_fragment(fmp4_t *s, wb_t *b, const unsigned char *avcc, size_t avcc_len,
                           int is_sync, uint64_t decode_time)
{
    size_t moof = box_begin(b, "moof");

    size_t mfhd = box_begin(b, "mfhd");
    wb_u32(b, 0); wb_u32(b, s->seq);
    box_end(b, mfhd);

    size_t traf = box_begin(b, "traf");

    size_t tfhd = box_begin(b, "tfhd");
    wb_u32(b, 0x020000);        /* default-base-is-moof */
    wb_u32(b, 1);               /* track_ID */
    box_end(b, tfhd);

    size_t tfdt = box_begin(b, "tfdt");
    wb_u32(b, 0x01000000);      /* version 1: 64 位 baseMediaDecodeTime */
    wb_u64(b, decode_time);
    box_end(b, tfdt);

    /* trun：data_offset 在 moof 结尾之后，稍后回填 */
    size_t trun = box_begin(b, "trun");
    wb_u32(b, 0x000701);        /* data-offset + duration + size + flags */
    wb_u32(b, 1);               /* sample_count */
    size_t data_off_pos = b->len;
    wb_u32(b, 0);               /* data_offset 占位 */
    wb_u32(b, 1);               /* sample_duration（timescale=fps）*/
    wb_u32(b, (uint32_t)avcc_len);
    wb_u32(b, is_sync ? 0x02000000 : 0x01010000);
    box_end(b, trun);

    box_end(b, traf);
    box_end(b, moof);

    uint32_t data_offset = (uint32_t)(b->len + 8 - moof);   /* mdat 头之后 */
    b->p[data_off_pos + 0] = (unsigned char)(data_offset >> 24);
    b->p[data_off_pos + 1] = (unsigned char)(data_offset >> 16);
    b->p[data_off_pos + 2] = (unsigned char)(data_offset >> 8);
    b->p[data_off_pos + 3] = (unsigned char)data_offset;

    size_t mdat = box_begin(b, "mdat");
    wb_data(b, avcc, avcc_len);
    box_end(b, mdat);
}

static int enqueue(fmp4_t *s, unsigned char *data, size_t len)
{
    if (s->count == FMP4_QUEUE) {                 /* 队列满：丢最旧 */
        fmp4_buf_t old = s->q[s->head];
        s->head = (s->head + 1) % FMP4_QUEUE;
        s->count--;
        free(old.data);
    }
    s->q[s->tail].data = data;
    s->q[s->tail].len = len;
    s->tail = (s->tail + 1) % FMP4_QUEUE;
    s->count++;
    return 0;
}

fmp4_t *fmp4_create(int w, int h, int fps)
{
    if (w <= 0 || h <= 0) return NULL;
    fmp4_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->w = w; s->h = h; s->fps = fps > 0 ? fps : 30;
    s->seq = 1;
    return s;
}

void fmp4_destroy(fmp4_t *s)
{
    if (!s) return;
    for (int i = 0; i < FMP4_QUEUE; i++) free(s->q[i].data);
    free(s->au);
    free(s);
}

int fmp4_write(fmp4_t *s, const unsigned char *annexb, size_t len)
{
    if (!s || !annexb || len < 5) return -1;
    struct au_scan a = { .sps = s->sps, .pps = s->pps, .sps_len = &s->sps_len, .pps_len = &s->pps_len };
    scan_nals(annexb, len, au_emit, &a);
    if (!a.len) return -1;

    if (!s->started) {
        if (!s->sps_len || !s->pps_len) { free(a.out); return -1; }
        wb_t init = {0};
        write_init(s, &init);
        enqueue(s, init.p, init.len);
        s->started = 1;
    }

    wb_t frag = {0};
    write_fragment(s, &frag, a.out, a.len, a.has_idr, s->samples);
    free(a.out);
    enqueue(s, frag.p, frag.len);

    s->seq++;
    s->samples += 1;
    return 0;
}

int fmp4_take(fmp4_t *s, const unsigned char **buf, size_t *len)
{
    if (!s || s->count == 0) return 0;
    fmp4_buf_t b = s->q[s->head];
    s->q[s->head].data = NULL;         /* 清空槽位，避免 destroy 时二次释放 */
    s->q[s->head].len = 0;
    s->head = (s->head + 1) % FMP4_QUEUE;
    s->count--;
    *buf = b.data;
    *len = b.len;
    return 1;              /* 缓冲所有权交给调用方，用完由它 free */
}

uint32_t fmp4_frames(const fmp4_t *s) { return s ? s->samples : 0; }
