/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * rec_mjpeg.c — 无损录像封装器：相机原生 JPEG 帧原样写入 AVI(MJPG)。
 *
 * 帧数据来自 V4L2 采集的 MJPEG 原始字节，录像期间不做解码、不做重编码，
 * 也不缩放、不重采样 —— 文件里的每一帧与传感器输出逐字节相同。
 *
 * 文件布局（RIFF 小端）：
 *   'RIFF' <size> 'AVI '
 *     'LIST' <size> 'hdrl'
 *        'avih' 56 <MainAVIHeader>
 *        'LIST' <size> 'strl'
 *           'strh' 56 <AVIStreamHeader>   fccType=vids fccHandler=MJPG
 *           'strf' 40 <BITMAPINFOHEADER>  biCompression=MJPG
 *     'LIST' <size> 'movi'
 *        '00dc' <len> <jpeg bytes>       每帧一个块，奇数长度补 1 字节对齐
 *     'idx1' <size> <AVIINDEXENTRY…>     每帧 16 字节，AVIIF_KEYFRAME
 *
 * 32 位偏移决定的实际上限约 2GB：按 720p 原生 JPEG ≈3.1MB/s 计约 10 分钟一段，
 * 到线后 write_frame 返回 -1，由录制线程收尾并续录下一段（既有分段机制）。
 *
 * 纯封装，无线程；调用方保证并发安全。
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>

#include "video/rec_mjpeg.h"

/* RIFF 各块长度字段是 uint32。留足余量后停写并分段，避免偏移回绕。 */
#define REC_MJ_MAX_BYTES (1900ull * 1024ull * 1024ull)
#define REC_MJ_INIT_CAP  1024u

/* 每帧索引项（ckid 固定 '00dc'，故只存 flags/offset/length） */
typedef struct {
    uint32_t flags;
    uint32_t offset;   /* 相对 movi 数据起点 */
    uint32_t length;   /* 不含对齐填充 */
} rec_mj_idx_t;

struct rec_mjpeg_s {
    FILE    *fp;
    char     path[512];
    char     name[128];
    int      w, h, fps;
    uint32_t n_frames;
    uint64_t data_bytes;         /* movi 数据累计（含块头与填充） */
    uint32_t max_frame;          /* 最大单帧，写 avih/strh 的 SuggestedBufferSize */
    long     movi_list_pos;      /* 'LIST' 四字符码位置 */
    long     movi_data_start;    /* 'movi' 之后，索引偏移的基准 */
    long     avih_pos;           /* 'avih' 数据起点，回填帧数/码率 */
    long     strh_pos;           /* 'strh' 数据起点，回填帧数 */
    rec_mj_idx_t *idx;
    uint32_t idx_cap;
    uint64_t start_ms;
};

static uint64_t mj_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

/* ---- 小端写入 ---- */

static void le32(FILE *fp, uint32_t v)
{
    fputc((int)(v & 0xff), fp);
    fputc((int)((v >> 8) & 0xff), fp);
    fputc((int)((v >> 16) & 0xff), fp);
    fputc((int)((v >> 24) & 0xff), fp);
}

static void le16(FILE *fp, uint16_t v)
{
    fputc((int)(v & 0xff), fp);
    fputc((int)((v >> 8) & 0xff), fp);
}

static void fourcc(FILE *fp, const char *s)
{
    fwrite(s, 1, 4, fp);
}

/* 回填此前用 le32 占位写下的长度字段 */
static void patch32(FILE *fp, long pos, uint32_t v)
{
    if (pos < 0) return;
    long cur = ftell(fp);
    if (cur < 0) return;
    if (fseek(fp, pos, SEEK_SET) == 0) le32(fp, v);
    fseek(fp, cur, SEEK_SET);
}

/* ---- 头部构造 ---- */

static void write_avih(FILE *fp, int w, int h, int fps, long *avih_pos)
{
    uint32_t us = (uint32_t)(fps > 0 ? (1000000 / fps) : 33333);
    fourcc(fp, "avih");
    le32(fp, 56);
    *avih_pos = ftell(fp);
    le32(fp, us);                              /* dwMicroSecPerFrame */
    le32(fp, 0);                               /* dwMaxBytesPerSec（finalize 回填） */
    le32(fp, 0);                               /* dwPaddingGranularity */
    le32(fp, 0x00000010u);                     /* dwFlags: AVIF_HASINDEX */
    le32(fp, 0);                               /* dwTotalFrames（finalize 回填） */
    le32(fp, 0);                               /* dwInitialFrames */
    le32(fp, 1);                               /* dwStreams */
    le32(fp, 0);                               /* dwSuggestedBufferSize（回填） */
    le32(fp, (uint32_t)w);                     /* dwWidth */
    le32(fp, (uint32_t)h);                     /* dwHeight */
    le32(fp, 0); le32(fp, 0); le32(fp, 0); le32(fp, 0);   /* dwReserved[4] */
}

static void write_strh(FILE *fp, int w, int h, int fps, long *strh_pos)
{
    fourcc(fp, "strh");
    le32(fp, 56);
    *strh_pos = ftell(fp);
    fourcc(fp, "vids");                        /* fccType */
    fourcc(fp, "MJPG");                        /* fccHandler */
    le32(fp, 0);                               /* dwFlags */
    le16(fp, 0);                               /* wPriority */
    le16(fp, 0);                               /* wLanguage */
    le32(fp, 0);                               /* dwInitialFrames */
    le32(fp, 1);                               /* dwScale */
    le32(fp, (uint32_t)(fps > 0 ? fps : 30));  /* dwRate */
    le32(fp, 0);                               /* dwStart */
    le32(fp, 0);                               /* dwLength（finalize 回填） */
    le32(fp, 0);                               /* dwSuggestedBufferSize（回填） */
    le32(fp, 0xFFFFFFFFu);                     /* dwQuality: 默认 */
    le32(fp, 0);                               /* dwSampleSize: 变长 */
    le16(fp, 0); le16(fp, 0);                  /* rcFrame.left/top */
    le16(fp, (uint16_t)w); le16(fp, (uint16_t)h);   /* rcFrame.right/bottom */
}

static void write_strf(FILE *fp, int w, int h)
{
    fourcc(fp, "strf");
    le32(fp, 40);
    le32(fp, 40);                              /* biSize */
    le32(fp, (uint32_t)w);                     /* biWidth */
    le32(fp, (uint32_t)h);                     /* biHeight（正数 = 自下而上；MJPEG 解码器按行序直读） */
    le16(fp, 1);                               /* biPlanes */
    le16(fp, 24);                              /* biBitCount */
    fourcc(fp, "MJPG");                        /* biCompression */
    le32(fp, (uint32_t)((uint64_t)w * (uint64_t)h * 3ull));   /* biSizeImage */
    le32(fp, 0);                               /* biXPelsPerMeter */
    le32(fp, 0);                               /* biYPelsPerMeter */
    le32(fp, 0);                               /* biClrUsed */
    le32(fp, 0);                               /* biClrImportant */
}

/* ---- 生命周期 ---- */

rec_mjpeg_t *rec_mjpeg_create(const char *dir, const char *prefix, int w, int h,
                              int fps, char *name_out, size_t name_size)
{
    if (!dir || !prefix || w <= 0 || h <= 0) return NULL;

    rec_mjpeg_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->fps = fps > 0 ? fps : 30;
    s->w = w;
    s->h = h;

    mkdir(dir, 0755);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    /* 完整创建时间戳进文件名，下载后仍保留录制时刻；EXCL 也避开快速分段重名 */
    for (int i = 0; i < 1000; i++) {
        int len;
        if (i == 0)
            len = snprintf(s->name, sizeof(s->name), "%s_%s_%03ld.avi",
                           prefix, stamp, tv.tv_usec / 1000);
        else
            len = snprintf(s->name, sizeof(s->name), "%s_%s_%03ld_%d.avi",
                           prefix, stamp, tv.tv_usec / 1000, i);
        if (len < 0 || (size_t)len >= sizeof(s->name)) break;
        len = snprintf(s->path, sizeof(s->path), "%s/%s", dir, s->name);
        if (len < 0 || (size_t)len >= sizeof(s->path)) break;
        int fd = open(s->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) { if (errno == EEXIST) continue; break; }
        s->fp = fdopen(fd, "wb");
        if (!s->fp) { close(fd); unlink(s->path); }
        break;
    }
    if (!s->fp) { free(s); return NULL; }

    s->idx_cap = REC_MJ_INIT_CAP;
    s->idx = malloc(s->idx_cap * sizeof(rec_mj_idx_t));
    if (!s->idx) {
        fclose(s->fp); unlink(s->path); free(s); return NULL;
    }
    s->start_ms = mj_now_ms();

    /* ---- RIFF / AVI ---- */
    fourcc(s->fp, "RIFF");
    le32(s->fp, 0);                    /* 文件总长 - 8，finalize 回填 */
    fourcc(s->fp, "AVI ");

    long hdrl_pos = ftell(s->fp) + 4;  /* 'LIST' 之后是 size 字段，再往后才是数据 */
    fourcc(s->fp, "LIST");
    le32(s->fp, 0);                    /* hdrl size，稍后回填 */
    fourcc(s->fp, "hdrl");
    long hdrl_data = ftell(s->fp);

    write_avih(s->fp, w, h, s->fps, &s->avih_pos);

    long strl_pos = ftell(s->fp) + 4;
    fourcc(s->fp, "LIST");
    le32(s->fp, 0);                    /* strl size */
    fourcc(s->fp, "strl");
    long strl_data = ftell(s->fp);
    write_strh(s->fp, w, h, s->fps, &s->strh_pos);
    write_strf(s->fp, w, h);
    /* A LIST size covers the form-type fourcc as part of the list content, so add
       back the 4 bytes the *_data bases skip past. */
    patch32(s->fp, strl_pos, (uint32_t)(ftell(s->fp) - strl_data) + 4u);
    patch32(s->fp, hdrl_pos, (uint32_t)(ftell(s->fp) - hdrl_data) + 4u);

    /* ---- movi ---- */
    s->movi_list_pos = ftell(s->fp);
    fourcc(s->fp, "LIST");
    le32(s->fp, 0);                    /* movi size，finalize 回填 */
    fourcc(s->fp, "movi");
    s->movi_data_start = ftell(s->fp);

    if (name_out && name_size) snprintf(name_out, name_size, "%s", s->name);
    return s;
}

int rec_mjpeg_write_frame(rec_mjpeg_t *s, const unsigned char *jpeg, size_t len,
                          uint64_t ts_ms)
{
    (void)ts_ms;
    if (!s || !s->fp || !jpeg || len == 0) return -1;
    if (len > 0x7FFFFFFFu) return -1;
    if (s->data_bytes + (uint64_t)len + 8ull > REC_MJ_MAX_BYTES) return -1;

    if (s->n_frames >= s->idx_cap) {
        uint32_t cap = s->idx_cap * 2u;
        rec_mj_idx_t *grown = realloc(s->idx, (size_t)cap * sizeof(*grown));
        if (!grown) return -1;
        s->idx = grown;
        s->idx_cap = cap;
    }

    long pos = ftell(s->fp);
    if (pos < 0) return -1;
    if ((uint64_t)(pos - s->movi_data_start) + (uint64_t)len + 8ull > 0xFFFFFFFFull) return -1;

    fourcc(s->fp, "00dc");
    le32(s->fp, (uint32_t)len);
    if (fwrite(jpeg, 1, len, s->fp) != len) {
        int saved = errno;
        /* A short write leaves a chunk whose declared length exceeds the bytes on
         * disk, which no parser can recover from and which would also corrupt the
         * movi size written at finalize. Rewind and truncate back to the chunk
         * header so the segment stays parseable. */
        (void)fflush(s->fp);
        (void)ftruncate(fileno(s->fp), pos);
        (void)fseeko(s->fp, pos, SEEK_SET);
        clearerr(s->fp);
        errno = saved;
        return -1;
    }
    if (len & 1u) fputc(0, s->fp);     /* RIFF 块按字对齐 */

    s->idx[s->n_frames].flags  = 0x00000010u;   /* AVIIF_KEYFRAME：每帧都是关键帧 */
    s->idx[s->n_frames].offset = (uint32_t)((long)(pos - s->movi_data_start));
    s->idx[s->n_frames].length = (uint32_t)len;
    s->n_frames++;
    s->data_bytes += (uint64_t)len + 8ull + (uint64_t)(len & 1u);
    if ((uint32_t)len > s->max_frame) s->max_frame = (uint32_t)len;
    return 0;
}

int rec_mjpeg_finalize(rec_mjpeg_t *s)
{
    if (!s) return -1;
    if (!s->fp) { free(s->idx); free(s); return -1; }

    int result = 0;
    if (s->n_frames == 0) {
        /* 无帧：不留下无意义的空文件 */
        fclose(s->fp);
        unlink(s->path);
        result = -1;
    } else {
        /* movi 长度：从 'LIST' 的 size 字段之后到当前写入点，含 'movi' 四字符码 */
        long movi_end = ftell(s->fp);
        patch32(s->fp, s->movi_list_pos + 4,
                (uint32_t)(movi_end - s->movi_data_start) + 4u);

        /* idx1：每帧 16 字节 */
        fourcc(s->fp, "idx1");
        le32(s->fp, s->n_frames * 16u);
        for (uint32_t i = 0; i < s->n_frames; i++) {
            fourcc(s->fp, "00dc");
            le32(s->fp, s->idx[i].flags);
            le32(s->fp, s->idx[i].offset);
            le32(s->fp, s->idx[i].length);
        }

        long file_end = ftell(s->fp);
        patch32(s->fp, 4, (uint32_t)(file_end - 8));      /* RIFF size */

        /* avih 字段偏移：dwMaxBytesPerSec=+4, dwTotalFrames=+16,
           dwSuggestedBufferSize=+28（见 write_avih 的写入顺序） */
        uint32_t el = (uint32_t)(s->start_ms && s->n_frames
                                 ? (mj_now_ms() - s->start_ms) : 0);
        uint32_t bps = el ? (uint32_t)((s->data_bytes * 1000ull) / el) : 0;
        patch32(s->fp, s->avih_pos + 4,  bps);
        patch32(s->fp, s->avih_pos + 16, s->n_frames);
        patch32(s->fp, s->avih_pos + 28, s->max_frame);

        /* strh 字段偏移：dwLength=+32, dwSuggestedBufferSize=+36
           （fccType/fccHandler/dwFlags/wPriority+wLanguage/dwInitialFrames/
             dwScale/dwRate/dwStart 共 32 字节） */
        patch32(s->fp, s->strh_pos + 32, s->n_frames);
        patch32(s->fp, s->strh_pos + 36, s->max_frame);

        if (fflush(s->fp) != 0) result = -1;
        if (fclose(s->fp) != 0) result = -1;
        if (result != 0) { /* 保留文件供排查，但不视为成功 */ }
    }

    free(s->idx);
    free(s);
    return result;
}

const char *rec_mjpeg_name(const rec_mjpeg_t *s)
{
    return s ? s->name : "";
}

uint32_t rec_mjpeg_frames(const rec_mjpeg_t *s)
{
    return s ? s->n_frames : 0;
}

uint32_t rec_mjpeg_bytes(const rec_mjpeg_t *s)
{
    return s ? (uint32_t)s->data_bytes : 0;
}

uint64_t rec_mjpeg_start_ms(const rec_mjpeg_t *s)
{
    return s ? s->start_ms : 0;
}
