/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * rec_mp4.c —— Annex-B H.264 到 MP4 的单遍封装。见 rec_mp4.h 的文件布局说明。
 */
#define _GNU_SOURCE
#include "video/rec_mp4.h"

#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"

#define MP4_MAX_NAL 32

typedef struct { uint32_t size; uint64_t offset; } sample_t;

struct rec_mp4 {
    FILE *fp;
    char path[512];
    int w, h, fps;
    uint32_t frames;
    uint64_t bytes;
    long mdat_size_pos;      /* mdat largesize 字段位置（8 字节）*/
    uint64_t mdat_data_pos;  /* 第一个样本的绝对偏移 */
    sample_t *samples;
    uint32_t sample_cap;
    unsigned char *au;       /* Annex-B → AVCC 转换缓冲 */
    size_t au_cap;
    unsigned char sps[512], pps[512];
    size_t sps_len, pps_len;
    int ready;
};

static void be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
}

static void put32(FILE *fp, uint32_t v) { unsigned char b[4]; be32(b, v); fwrite(b, 1, 4, fp); }
static void put64(FILE *fp, uint64_t v) { put32(fp, (uint32_t)(v >> 32)); put32(fp, (uint32_t)v); }
static void putcc(FILE *fp, const char *s) { fwrite(s, 1, 4, fp); }
static void put8(FILE *fp, unsigned v) { fputc((int)v, fp); }
static void put16(FILE *fp, unsigned v) { put8(fp, v >> 8); put8(fp, v & 0xff); }
static void putzeros(FILE *fp, int n) { for (int i = 0; i < n; i++) put8(fp, 0); }

/* 开一个盒子，返回 size 字段位置；结束时用 box_end 回填 */
static long box_begin(FILE *fp, const char *type)
{
    long pos = ftell(fp);
    put32(fp, 0);
    putcc(fp, type);
    return pos;
}

static void box_end(FILE *fp, long pos)
{
    long here = ftell(fp);
    fseek(fp, pos, SEEK_SET);
    put32(fp, (uint32_t)(here - pos));
    fseek(fp, here, SEEK_SET);
}

/* 遍历 Annex-B：回调每段 NAL（不含起始码） */
static void scan_nals(const unsigned char *buf, size_t len,
                      void (*cb)(void *, const unsigned char *, size_t), void *ctx)
{
    size_t i = 0;
    while (i + 3 <= len) {
        size_t start = i, payload;
        if (buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 1) payload = i + 3;
        else if (i + 4 <= len && buf[i] == 0 && buf[i+1] == 0 && buf[i+2] == 0 && buf[i+3] == 1) payload = i + 4;
        else { i++; continue; }
        size_t j = payload;
        while (j + 3 <= len) {
            if (buf[j] == 0 && buf[j+1] == 0 &&
                (buf[j+2] == 1 || (j + 4 <= len && buf[j+2] == 0 && buf[j+3] == 1))) break;
            j++;
        }
        size_t end = (j + 3 <= len) ? j : len;
        if (end > payload) cb(ctx, buf + payload, end - payload);
        i = (end > payload) ? end : payload;
        (void)start;
    }
}

struct grab { unsigned char *sps; size_t *sps_len; unsigned char *pps; size_t *pps_len; };

static void grab_param_sets(void *ctx, const unsigned char *nal, size_t len)
{
    struct grab *g = ctx;
    if (!len) return;
    int type = nal[0] & 0x1f;
    if (type == 7 && len <= 512 && !*g->sps_len) { memcpy(g->sps, nal, len); *g->sps_len = len; }
    if (type == 8 && len <= 512 && !*g->pps_len) { memcpy(g->pps, nal, len); *g->pps_len = len; }
}

/* Annex-B → AVCC（4 字节长度前缀），返回写出的字节数 */
struct avcc { unsigned char *out; size_t cap, len; };

static void emit_avcc(void *ctx, const unsigned char *nal, size_t len)
{
    struct avcc *a = ctx;
    if (a->len + 4 + len > a->cap) return;
    be32(a->out + a->len, (uint32_t)len);
    a->len += 4;
    memcpy(a->out + a->len, nal, len);
    a->len += len;
}

/* ---- moov 构建 ---- */

static void write_ftyp(FILE *fp)
{
    long b = box_begin(fp, "ftyp");
    putcc(fp, "isom"); put32(fp, 0x200);
    putcc(fp, "isom"); putcc(fp, "iso2"); putcc(fp, "avc1"); putcc(fp, "mp41");
    box_end(fp, b);
}

static void write_avcC(FILE *fp, const rec_mp4_t *s)
{
    put8(fp, 1);                                   /* configurationVersion */
    put8(fp, s->sps_len > 1 ? s->sps[1] : 0x64);   /* AVCProfileIndication */
    put8(fp, s->sps_len > 2 ? s->sps[2] : 0);      /* profile_compatibility */
    put8(fp, s->sps_len > 3 ? s->sps[3] : 0x28);   /* AVCLevelIndication */
    put8(fp, 0xFF);                                /* lengthSizeMinusOne = 3 */
    put8(fp, 0xE1);                                /* numOfSPS = 1 */
    put16(fp, (unsigned)s->sps_len);
    fwrite(s->sps, 1, s->sps_len, fp);
    put8(fp, 1);                                   /* numOfPPS */
    put16(fp, (unsigned)s->pps_len);
    fwrite(s->pps, 1, s->pps_len, fp);
}

static void write_moov(rec_mp4_t *s)
{
    FILE *fp = s->fp;
    uint32_t n = s->frames;
    uint32_t dur = n;                      /* timescale == fps，样本时长 1 */

    long moov = box_begin(fp, "moov");

    /* mvhd (version 0) */
    long b = box_begin(fp, "mvhd");
    put32(fp, 0); put32(fp, 0); put32(fp, 0);           /* version/flags, ctime, mtime */
    put32(fp, (uint32_t)s->fps); put32(fp, dur);        /* timescale, duration */
    put32(fp, 0x00010000); put16(fp, 0x0100); putzeros(fp, 10);
    for (int i = 0; i < 9; i++) put32(fp, i == 0 || i == 4 || i == 8 ? 0x00010000 : 0);
    putzeros(fp, 24);                                   /* pre_defined */
    put32(fp, 2);                                       /* next_track_ID */
    box_end(fp, b);

    long trak = box_begin(fp, "trak");

    /* tkhd */
    b = box_begin(fp, "tkhd");
    put32(fp, 7);                                       /* version 0, flags 7 */
    put32(fp, 0); put32(fp, 0); put32(fp, 1); put32(fp, 0);
    put32(fp, dur);
    putzeros(fp, 8); put16(fp, 0); put16(fp, 0); put16(fp, 0); put16(fp, 0);
    for (int i = 0; i < 9; i++) put32(fp, i == 0 || i == 4 || i == 8 ? 0x00010000 : 0);
    put32(fp, (uint32_t)s->w << 16); put32(fp, (uint32_t)s->h << 16);
    box_end(fp, b);

    long mdia = box_begin(fp, "mdia");

    /* mdhd */
    b = box_begin(fp, "mdhd");
    put32(fp, 0); put32(fp, 0); put32(fp, 0);
    put32(fp, (uint32_t)s->fps); put32(fp, dur);
    put16(fp, 0x55C4);                                  /* 'und' */
    put16(fp, 0);
    box_end(fp, b);

    /* hdlr */
    b = box_begin(fp, "hdlr");
    put32(fp, 0); put32(fp, 0); putcc(fp, "vide");
    putzeros(fp, 12);
    fwrite("VideoHandler", 1, 12, fp); put8(fp, 0);
    box_end(fp, b);

    long minf = box_begin(fp, "minf");

    b = box_begin(fp, "vmhd");
    put32(fp, 1); put16(fp, 0); put16(fp, 0); put16(fp, 0); put16(fp, 0);
    box_end(fp, b);

    b = box_begin(fp, "dinf");
    long dref = box_begin(fp, "dref");
    put32(fp, 0); put32(fp, 1);
    long url = box_begin(fp, "url ");
    put32(fp, 1);                                       /* flags=1: self-contained */
    box_end(fp, url);
    box_end(fp, dref);
    box_end(fp, b);

    long stbl = box_begin(fp, "stbl");

    /* stsd */
    b = box_begin(fp, "stsd");
    put32(fp, 0); put32(fp, 1);
    long avc1 = box_begin(fp, "avc1");
    putzeros(fp, 6); put16(fp, 1);                       /* reserved, data_reference_index */
    putzeros(fp, 16);                                    /* pre_defined/reserved */
    put16(fp, (unsigned)s->w); put16(fp, (unsigned)s->h);
    put32(fp, 0x00480000); put32(fp, 0x00480000);        /* 72 dpi */
    put32(fp, 0); put16(fp, 1);                          /* reserved, frame_count */
    putzeros(fp, 32);                                    /* compressorname */
    put16(fp, 24); put16(fp, 0xFFFF);                    /* depth, pre_defined */
    long avcC = box_begin(fp, "avcC");
    write_avcC(fp, s);
    box_end(fp, avcC);
    box_end(fp, avc1);
    box_end(fp, b);

    /* stts：所有样本同长 */
    b = box_begin(fp, "stts");
    put32(fp, 0); put32(fp, 1); put32(fp, n); put32(fp, 1);
    box_end(fp, b);

    /* stsc：全部样本在一个 chunk 里 */
    b = box_begin(fp, "stsc");
    put32(fp, 0); put32(fp, 1); put32(fp, 1); put32(fp, n); put32(fp, 1);
    box_end(fp, b);

    /* stsz */
    b = box_begin(fp, "stsz");
    put32(fp, 0); put32(fp, 0); put32(fp, n);
    for (uint32_t i = 0; i < n; i++) put32(fp, s->samples[i].size);
    box_end(fp, b);

    /* co64：64 位偏移，单 chunk */
    b = box_begin(fp, "co64");
    put32(fp, 0); put32(fp, 1);
    put64(fp, n ? s->samples[0].offset : s->mdat_data_pos);
    box_end(fp, b);

    box_end(fp, stbl);
    box_end(fp, minf);
    box_end(fp, mdia);
    box_end(fp, trak);
    box_end(fp, moov);
}

rec_mp4_t *rec_mp4_create_named(const char *dir, const char *name, int w, int h, int fps)
{
    if (!dir || !name || w <= 0 || h <= 0) return NULL;
    rec_mp4_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->w = w; s->h = h;
    s->fps = fps > 0 ? fps : 30;
    /* 会话目录按日期分层，逐级建出来 */
    {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s", dir);
        for (char *q = tmp + 1; *q; q++) {
            if (*q != '/') continue;
            *q = '\0';
            mkdir(tmp, 0755);
            *q = '/';
        }
        mkdir(tmp, 0755);
    }
    snprintf(s->path, sizeof(s->path), "%s/%s", dir, name);
    s->fp = fopen(s->path, "wb");
    if (!s->fp) {
        LOG_ERROR("rec_mp4: 打开 %s 失败: %s", s->path, strerror(errno));
        free(s);
        return NULL;
    }
    LOG_INFO("rec_mp4: 标注录像 %s（H.264/MP4）", s->path);
    return s;
}

static int start_mdat(rec_mp4_t *s)
{
    write_ftyp(s->fp);
    put32(s->fp, 1);                 /* mdat，用 64 位 largesize */
    putcc(s->fp, "mdat");
    s->mdat_size_pos = ftell(s->fp);
    put64(s->fp, 0);                 /* 占位 */
    s->mdat_data_pos = (uint64_t)ftell(s->fp);
    return 0;
}

int rec_mp4_write(rec_mp4_t *s, const unsigned char *annexb, size_t len)
{
    if (!s || !s->fp || !annexb || len < 5) return -1;

    if (!s->ready) {
        struct grab g = { s->sps, &s->sps_len, s->pps, &s->pps_len };
        scan_nals(annexb, len, grab_param_sets, &g);
        if (!s->sps_len || !s->pps_len) {
            static int warned;
            if (!warned++) LOG_ERROR("rec_mp4: 首帧缺少 SPS/PPS，等待下一个关键帧");
            return -1;
        }
        start_mdat(s);
        s->ready = 1;
    }

    size_t need = len + MP4_MAX_NAL * 4 + 64;
    if (need > s->au_cap) {
        unsigned char *grown = realloc(s->au, need);
        if (!grown) return -1;
        s->au = grown; s->au_cap = need;
    }
    struct avcc a = { s->au, s->au_cap, 0 };
    scan_nals(annexb, len, emit_avcc, &a);
    if (!a.len) return -1;

    if (s->frames == s->sample_cap) {
        uint32_t cap = s->sample_cap ? s->sample_cap * 2 : 256;
        sample_t *grown = realloc(s->samples, (size_t)cap * sizeof(*grown));
        if (!grown) return -1;
        s->samples = grown; s->sample_cap = cap;
    }
    long pos = ftell(s->fp);
    if (pos < 0) return -1;
    s->samples[s->frames].offset = (uint64_t)pos;
    s->samples[s->frames].size = (uint32_t)a.len;
    if (fwrite(a.out, 1, a.len, s->fp) != a.len) return -1;
    s->frames++;
    s->bytes += a.len;
    return 0;
}

int rec_mp4_close(rec_mp4_t *s)
{
    if (!s) return -1;
    int rc = 0;
    if (s->fp && s->ready) {
        uint64_t mdat_end = (uint64_t)ftell(s->fp);     /* moov 开始处 = mdat 结束 */
        write_moov(s);
        long end = ftell(s->fp);
        uint64_t box_start = (uint64_t)s->mdat_size_pos - 8;   /* 4 size + 4 fourcc */
        fseek(s->fp, s->mdat_size_pos, SEEK_SET);
        put64(s->fp, mdat_end - box_start);
        fseek(s->fp, end, SEEK_SET);
        fflush(s->fp);
        fsync(fileno(s->fp));
        LOG_INFO("rec_mp4: 收尾 %u 帧, %llu 字节", s->frames, (unsigned long long)s->bytes);
    }
    if (s->fp && fclose(s->fp) != 0) rc = -1;
    free(s->samples);
    free(s->au);
    free(s);
    return rc;
}
