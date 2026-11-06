/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * video_record.c —— 标注录像：NV12 画框 + MPP 硬件 H.264 + MP4 封装。
 *
 * 帧来自推理路径已经解好的 NV12 dma-buf；入队时拷一份（1.4 MB / 720p，约
 * 0.3 ms），之后 fd 可以立刻归还，避免和推理抢解码器缓冲。
 */
#define _GNU_SOURCE
#include "video/video_record.h"

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
#include "video/rec_font.h"
#include "video/rec_mp4.h"

#define ANNOT_QUEUE_CAP  12
#define ANNOT_MAX_BOXES  32

/* 帧缓冲环。MPP 是异步读输入缓冲的：put_frame 返回时它可能还在读这一帧，而
 * 这一帧又会成为后续 P 帧的参考帧。所以缓冲不能"用完就复用"——写进去的下一
 * 帧会和上一帧的像素混在一起。画面主体几乎不变，看着还正常；变化的部分（框
 * 线、标签文字）会被撕成断续的虚线，这就是"画框异常、没有标签"的根因。
 *
 * 回收只按帧龄算：某个缓冲承载的帧比"最新提交的帧"旧了 ANNOT_BUF_KEEP 帧才
 * 允许复用。按帧龄而不是按码流包计数，是因为一帧可能拆成多个 VCL 包吐出，用
 * 包计数会把还挂在编码器里的缓冲提前放掉。 */
#define ANNOT_BUF_COUNT  12
#define ANNOT_BUF_KEEP   8

/* 恒定量化步长。FIXQP 模式下画质只由这一个数决定：
 *   18 —— 比 20 再细一档（约 1.5 倍码率），720p30 的细线与文字边缘更干净；
 *   每降 6 相当于码率翻倍，16 以下肉眼收益很小，24 则明显发糊。
 * 想换画质/换体积只改这里。 */
#define ANNOT_FIX_QP     18

typedef struct {
    unsigned char *nv12;
    int w, h;
    int count;
    video_record_box_t boxes[ANNOT_MAX_BOXES];
} annot_frame_t;

struct video_record {
    pthread_t tid;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int running;

    annot_frame_t q[ANNOT_QUEUE_CAP];
    int head, tail, count;

    char dir[256], prefix[64];
    char name[128];               /* 会话开始就定好文件名，MP4 按名创建 */
    int w, h, fps, bitrate_kbps;
    int hor_stride, ver_stride;   /* MPP 要求的 16 对齐行距/面距，与图像宽高区分 */

    rec_mp4_t *mp4;

    MppCtx enc_ctx;
    MppApi *enc_mpi;
    MppBufferGroup grp;

    MppBuffer buf[ANNOT_BUF_COUNT];
    int  buf_frame[ANNOT_BUF_COUNT];  /* 该缓冲承载的帧序号，-1 表示空闲 */
    unsigned put_count;               /* 已交给编码器的帧数（单调递增） */

    int enc_ready;

    atomic_ullong frames, dropped, bytes_total;
    atomic_uint last_ms;
    atomic_int   disabled;        /* 硬件不可用时置位，永不重试 */
};

/* 框/标签的取色：从调色板按类别索引取色。对同一类别全程恒定（视频里不闪、
 * 也不给编码器制造无谓的色度变化），跨类别尽量岔开，观感就是"随机彩色"。
 * 值是满量程 YUV（编码器 prep:range 用 JPEG 全量程），按 BT.601 从 RGB 换算。 */
typedef struct {
    unsigned char y, u, v;
} annot_color_t;

/* 文字的两档亮度（色度恒中性，靠 Y 出黑/白） */
#define ANNOT_DARK   16
#define ANNOT_LIGHT  235

static const annot_color_t annot_palette[] = {
    { 84, 114, 225},  /* 胭脂红 */
    {158,  39, 197},  /* 橙 */
    {205,  12, 163},  /* 黄 */
    {127, 102,  38},  /* 绿 */
    {133, 160,  33},  /* 青 */
    { 91, 221,  85},  /* 蓝 */
    {112, 209, 162},  /* 紫 */
    {132, 155, 216},  /* 品红 */
    {195,  52,  81},  /* 黄绿 */
    { 98, 152,  58},  /* 水鸭 */
    {111,  88, 177},  /* 棕 */
    {184, 120, 179},  /* 粉 */
};
#define ANNOT_COLOR_N (int)(sizeof(annot_palette) / sizeof(annot_palette[0]))

static const annot_color_t *annot_color(int cls)
{
    return &annot_palette[(unsigned)(cls > 0 ? cls : 0) % ANNOT_COLOR_N];
}

/* 文字用中性色度（u=v=128），才不会被标签底色染色；亮度按底色取反差色。 */
#define ANNOT_TEXT_U 128
#define ANNOT_TEXT_V 128
static unsigned char annot_text_y(const annot_color_t *c)
{
    return c->y >= 140 ? ANNOT_DARK : ANNOT_LIGHT;
}

/* 画一个空心矩形。w 是图像宽度，stride 是缓冲行距（MPP 要求 16 对齐，可能与
 * w 不等）。两者混用就会出现"斜条纹/花屏"——每行错开 fixed 字节，越往下越斜。
 * Y 平面画线，UV 平面按 4:2:0 半分辨率再描一遍同区域，颜色才出得来。 */
static void draw_rect(unsigned char *y, unsigned char *uv, int w, int h, int stride,
                      int x1, int y1, int x2, int y2, int thickness,
                      const annot_color_t *c)
{
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > w - 1) x2 = w - 1;
    if (y2 > h - 1) y2 = h - 1;
    if (x2 - x1 < 1 || y2 - y1 < 1) return;

    for (int t = 0; t < thickness; t++) {
        int yt = y1 + t, yb = y2 - t;
        if (yt > yb) break;
        size_t row = (size_t)yt * (size_t)stride;
        memset(y + row + x1, c->y, (size_t)(x2 - x1 + 1));
        if (yb != yt) {
            row = (size_t)yb * (size_t)stride;
            memset(y + row + x1, c->y, (size_t)(x2 - x1 + 1));
        }
        int xl = x1 + t, xr = x2 - t;
        if (xl > xr) break;
        for (int yy = yt; yy <= yb; yy++) {
            unsigned char *line = y + (size_t)yy * (size_t)stride;
            line[xl] = c->y;
            line[xr] = c->y;
        }
    }

    /* 色度半分辨率：坐标/线宽折半，覆盖同一个环带 */
    int cx1 = x1 >> 1, cy1 = y1 >> 1, cx2 = x2 >> 1, cy2 = y2 >> 1;
    int ct = (thickness + 1) >> 1;
    if (cx2 - cx1 < 1 || cy2 - cy1 < 1) return;
    for (int t = 0; t < ct; t++) {
        int yt = cy1 + t, yb = cy2 - t;
        if (yt > yb) break;
        unsigned char *row = uv + (size_t)yt * (size_t)stride;
        for (int x = cx1; x <= cx2; x++) { row[x*2] = c->u; row[x*2+1] = c->v; }
        if (yb != yt) {
            row = uv + (size_t)yb * (size_t)stride;
            for (int x = cx1; x <= cx2; x++) { row[x*2] = c->u; row[x*2+1] = c->v; }
        }
        int xl = cx1 + t, xr = cx2 - t;
        if (xl > xr) break;
        for (int yy = yt; yy <= yb; yy++) {
            unsigned char *row2 = uv + (size_t)yy * (size_t)stride;
            row2[xl*2] = c->u; row2[xl*2+1] = c->v;
            row2[xr*2] = c->u; row2[xr*2+1] = c->v;
        }
    }
}

/* 标签：与框同色的底条 + 反差色点阵字，贴在框左上角外侧；框贴顶时改放框内。 */
static void draw_label(unsigned char *y, unsigned char *uv, int w, int h, int stride,
                       int bx1, int by1, const char *text, const annot_color_t *bar)
{
    if (!text || !text[0]) return;
    size_t len = strlen(text);
    if (len > 24) len = 24;
    int tw = (int)len * REC_FONT_W;
    int bar_w = tw + 6, bar_h = REC_FONT_H + 4;
    if (bar_w > w) return;

    int x0 = bx1 < 0 ? 0 : bx1;
    if (x0 + bar_w > w) x0 = w - bar_w;
    int y0 = by1 - bar_h;
    if (y0 < 0) y0 = by1 + 2;
    if (y0 + bar_h > h) y0 = h - bar_h;
    if (x0 < 0 || y0 < 0) return;

    unsigned char text_y = annot_text_y(bar);

    for (int yy = y0; yy < y0 + bar_h; yy++)
        memset(y + (size_t)yy * (size_t)stride + x0, bar->y, (size_t)bar_w);
    int cx0 = x0 >> 1, cy0 = y0 >> 1;
    int ccols = ((x0 + bar_w - 1) >> 1) - cx0 + 1;
    int crows = ((y0 + bar_h - 1) >> 1) - cy0 + 1;
    for (int r = 0; r < crows; r++) {
        unsigned char *row = uv + (size_t)(cy0 + r) * (size_t)stride;
        for (int x = 0; x < ccols; x++) { row[(cx0 + x)*2] = bar->u; row[(cx0 + x)*2+1] = bar->v; }
    }

    for (size_t i = 0; i < len; i++) {
        int code = (unsigned char)text[i];
        if (code < REC_FONT_FIRST || code >= REC_FONT_FIRST + 95) code = '?';
        const unsigned short *glyph = rec_font[code - REC_FONT_FIRST];
        int gx = x0 + 3 + (int)i * REC_FONT_W;
        for (int r = 0; r < REC_FONT_H; r++) {
            unsigned short bits = glyph[r];
            if (!bits) continue;
            unsigned char *line = y + (size_t)(y0 + 2 + r) * (size_t)stride;
            unsigned char *uvline = uv + (size_t)((y0 + 2 + r) >> 1) * (size_t)stride;
            for (int c = 0; c < REC_FONT_W; c++)
                if (bits & (1u << (REC_FONT_W - 1 - c))) {
                    int px = gx + c;
                    line[px] = text_y;
                    uvline[(px >> 1) * 2]     = ANNOT_TEXT_U;
                    uvline[(px >> 1) * 2 + 1] = ANNOT_TEXT_V;
                }
        }
    }
}

/* 画一个检测框：单一彩色描边（颜色按类别），左上角贴同色标签条。 */
static void draw_box_nv12(unsigned char *y, unsigned char *uv, int w, int h, int stride,
                          const video_record_box_t *b, int thickness)
{
    int x1 = (int)(b->x1 + 0.5f), y1 = (int)(b->y1 + 0.5f);
    int x2 = (int)(b->x2 + 0.5f), y2 = (int)(b->y2 + 0.5f);
    if (x2 - x1 < 2 || y2 - y1 < 2) return;
    const annot_color_t *c = annot_color(b->cls);
    draw_rect(y, uv, w, h, stride, x1, y1, x2, y2, thickness, c);
    draw_label(y, uv, w, h, stride, x1 < 0 ? 0 : x1, y1 < 0 ? 0 : y1, b->label, c);
}

static int encoder_open(video_record_t *s)
{
    if (mpp_create(&s->enc_ctx, &s->enc_mpi) != MPP_OK) return -1;
    if (mpp_init(s->enc_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) return -1;

    MppEncCfg cfg = NULL;
    if (mpp_enc_cfg_init(&cfg) != MPP_OK) return -1;
    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "prep:width", s->w);
    mpp_enc_cfg_set_s32(cfg, "prep:height", s->h);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", s->hor_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", s->ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "prep:range", MPP_FRAME_RANGE_JPEG);

    /* 恒定 QP：不做码率控制，画质只由 ANNOT_FIX_QP 决定，体积随画面复杂度浮动。
     * 之前是 VBR —— RC 的职责是"把目标码率花完"，画面简单时它不会主动降 QP
     * 去换画质，于是 I 帧长期停在 qp_max_i(30)，细线被打断、边缘起振铃。
     * FIXQP 下 rc_model_v2 直接把 init_quality 当 quality_target，I/P 同值。 */
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_FIXQP);
    mpp_enc_cfg_set_u32(cfg, "rc:max_reenc_times", 0);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    /* FIXQP 不看码率，但 MPP 仍用 bps_target 算 bit_per_frame / gop_total_bits，
     * 留 0 有除零风险；这里只作名义值，实际码率由 QP 决定 */
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
    /* 五项都给同一个值：qp_min/max 只是边界，FIXQP 真正取的是 qp_init */
    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", ANNOT_FIX_QP);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", ANNOT_FIX_QP);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", ANNOT_FIX_QP);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", ANNOT_FIX_QP);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", ANNOT_FIX_QP);
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
    /* 4.1：固定 QP 后码率没有上限，High@4.0 在 720p30 只允许 20 Mbps */
    mpp_enc_cfg_set_s32(cfg, "h264:level", 41);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    if (s->enc_mpi->control(s->enc_ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
        mpp_enc_cfg_deinit(cfg);
        return -1;
    }
    mpp_enc_cfg_deinit(cfg);
    /* 每个 IDR 重发 SPS/PPS：分段重建 muxer 时需要参数集才能生成 avcC。
     * 原先这段被写在 SET_CFG 的失败分支里，成功路径从来没执行过。 */
    {
        MppEncHeaderMode hm = MPP_ENC_HEADER_MODE_EACH_IDR;
        if (s->enc_mpi->control(s->enc_ctx, MPP_ENC_SET_HEADER_MODE, &hm) != MPP_OK)
            LOG_ERROR("video_record: 无法启用逐 IDR 参数集");
    }
    if (mpp_buffer_group_get_internal(&s->grp, MPP_BUFFER_TYPE_DRM) != MPP_OK) return -1;
    for (int i = 0; i < ANNOT_BUF_COUNT; i++) {
        if (mpp_buffer_get(s->grp, &s->buf[i],
                           (size_t)s->hor_stride * s->ver_stride * 3 / 2) != MPP_OK)
            return -1;
        s->buf_frame[i] = -1;
    }
    s->enc_ready = 1;
    return 0;
}

/* 取一个空闲缓冲，并给它打上帧序号。返回 -1 表示环形已排满：此时宁可丢掉
 * 这一帧，也不能覆盖编码器可能还在读的缓冲。 */
static int buf_acquire(video_record_t *s)
{
    /* 先按帧龄把老缓冲放回池子 */
    if (s->put_count > ANNOT_BUF_KEEP) {
        unsigned limit = s->put_count - ANNOT_BUF_KEEP;
        for (int i = 0; i < ANNOT_BUF_COUNT; i++)
            if (s->buf_frame[i] >= 0 && (unsigned)s->buf_frame[i] < limit)
                s->buf_frame[i] = -1;
    }
    for (int i = 0; i < ANNOT_BUF_COUNT; i++)
        if (s->buf_frame[i] < 0) {
            s->buf_frame[i] = (int)s->put_count++;
            return i;
        }
    return -1;
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

static void annot_feed(video_record_t *s, const unsigned char *p, size_t len, int vcl)
{
    if (!vcl) {
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

static void drain_packets(video_record_t *s)
{
    for (;;) {
        MppPacket pkt = NULL;
        if (s->enc_mpi->encode_get_packet(s->enc_ctx, &pkt) != MPP_OK || !pkt) break;
        const unsigned char *ptr = mpp_packet_get_pos(pkt);
        size_t len = mpp_packet_get_length(pkt);
        int eos = mpp_packet_get_eos(pkt);
        int vcl = len ? pkt_has_vcl(ptr, len) : 0;
        if (len && s->mp4) annot_feed(s, ptr, len, vcl);
        if (len) atomic_fetch_add(&s->bytes_total, len);
        mpp_packet_deinit(&pkt);
        if (eos) break;
    }
}

static int encode_frame(video_record_t *s, annot_frame_t *f)
{
    if (!s->mp4) {
        s->mp4 = rec_mp4_create_named(s->dir, s->name, s->w, s->h, s->fps);
        if (!s->mp4) return -1;
    }
    if (!s->enc_ready && encoder_open(s) != 0) {
        if (s->enc_ctx) { mpp_destroy(s->enc_ctx); s->enc_ctx = NULL; }
        if (s->grp) { mpp_buffer_group_put(s->grp); s->grp = NULL; }
        for (int i = 0; i < ANNOT_BUF_COUNT; i++)
            if (s->buf[i]) { mpp_buffer_put(s->buf[i]); s->buf[i] = NULL; }
        atomic_store(&s->disabled, 1);
        LOG_ERROR("video_record: 硬件编码器不可用，本次运行停用标注录像");
        return -1;
    }
    if (f->w != s->w || f->h != s->h) return -1;

    int slot = buf_acquire(s);
    if (slot < 0) { drain_packets(s); slot = buf_acquire(s); }
    if (slot < 0) return -1;
    MppBuffer frame_buf = s->buf[slot];
    unsigned char *dst = mpp_buffer_get_ptr(frame_buf);
    /* dma-buf 若是带 cache 的映射，CPU 写完必须做一次 cache 回写，否则 VEPU
       通过 DMA 读到的可能是旧行——又是一种"文字被啃掉"的来源。START/END 把
       这段写入包起来；非 dma-buf 缓冲（fd < 0）或已经 uncached 时是空操作。 */
    int fd = mpp_buffer_get_fd(frame_buf);
    struct dma_buf_sync sync;
    if (fd >= 0) {
        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) { /* 忽略：不支持同步的缓冲 */ }
    }
    /* 源是紧凑 NV12（行距 = w），目标是 stride 行距的缓冲：逐行拷，不能整块 memcpy */
    const unsigned char *src = f->nv12;
    for (int y = 0; y < s->h; y++)
        memcpy(dst + (size_t)y * s->hor_stride, src + (size_t)y * s->w, (size_t)s->w);
    unsigned char *dst_uv = dst + (size_t)s->hor_stride * s->ver_stride;
    const unsigned char *src_uv = src + (size_t)s->w * s->h;
    for (int y = 0; y < s->h / 2; y++)
        memcpy(dst_uv + (size_t)y * s->hor_stride, src_uv + (size_t)y * s->w, (size_t)s->w);
    for (int i = 0; i < f->count; i++)
        draw_box_nv12(dst, dst_uv, s->w, s->h, s->hor_stride, &f->boxes[i], 3);
    if (fd >= 0) {
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
        if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) { /* 同上 */ }
    }

    MppFrame out = NULL;
    if (mpp_frame_init(&out) != MPP_OK) return -1;
    mpp_frame_set_width(out, s->w);
    mpp_frame_set_height(out, s->h);
    mpp_frame_set_hor_stride(out, s->hor_stride);
    mpp_frame_set_ver_stride(out, s->ver_stride);
    mpp_frame_set_fmt(out, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(out, frame_buf);
    int rc = (s->enc_mpi->encode_put_frame(s->enc_ctx, out) == MPP_OK) ? 0 : -1;
    mpp_frame_deinit(&out);
    if (rc) return -1;
    drain_packets(s);
    /* 分段交给 video_rec：这里直接换名会截断同名文件，而且外部不知道名字变了 */
    return 0;
}

static void *annot_task(void *arg)
{
    video_record_t *s = arg;
    pthread_setname_np(pthread_self(), "video_record");
    for (;;) {
        pthread_mutex_lock(&s->lock);
        while (s->running && s->count == 0) pthread_cond_wait(&s->wake, &s->lock);
        if (!s->running && s->count == 0) { pthread_mutex_unlock(&s->lock); break; }
        annot_frame_t f = s->q[s->head];
        s->q[s->head].nv12 = NULL;      /* 槽位已交出，stop() 不能再 free 一次 */
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
        for (int i = 0; i < ANNOT_BUF_COUNT; i++)
            if (s->buf[i]) { mpp_buffer_put(s->buf[i]); s->buf[i] = NULL; }
        if (s->grp) { mpp_buffer_group_put(s->grp); s->grp = NULL; }
        mpp_destroy(s->enc_ctx);
    }
    if (s->mp4) { rec_mp4_close(s->mp4); s->mp4 = NULL; }
    LOG_INFO("video_record: 线程退出，已编码 %llu 帧",
             (unsigned long long)atomic_load(&s->frames));
    return NULL;
}

video_record_t *video_record_start(const char *dir, const char *prefix, int w, int h, int fps,
                             int bitrate_kbps, char *name_out, size_t name_size)
{
    if (!dir || !prefix || w <= 0 || h <= 0) return NULL;
    video_record_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    snprintf(s->dir, sizeof(s->dir), "%s", dir);
    snprintf(s->prefix, sizeof(s->prefix), "%s", prefix);
    {
        /* 一天一个文件：video_YYYYMMDD.mp4。同一天重开（重启或手动收尾）
           时加 _1、_2 后缀，避免覆盖当天已经录好的内容。 */
        time_t now = time(NULL);
        struct tm tm;
        char when[32], path[512];
        localtime_r(&now, &tm);
        strftime(when, sizeof(when), "%Y%m%d", &tm);
        snprintf(s->name, sizeof(s->name), "%s_%s.mp4", prefix, when);
        for (int index = 1; index < 1000; index++) {
            snprintf(path, sizeof(path), "%s/%s", s->dir, s->name);
            if (access(path, F_OK) != 0) break;
            snprintf(s->name, sizeof(s->name), "%s_%s_%d.mp4", prefix, when, index);
        }
        if (name_out && name_size) snprintf(name_out, name_size, "%s", s->name);
    }
    s->w = w; s->h = h;
    /* MPP 硬件编码器按 16 对齐取行距/面距。1280x720 恰好对齐时两者相等，
       但换成 1280x714 这类尺寸就会错开，每行错位累加成斜条纹/花屏。 */
    s->hor_stride = (w + 15) & ~15;
    s->ver_stride = (h + 15) & ~15;
    s->fps = fps > 0 ? fps : 30;
    /* 码率不再决定画质（FIXQP），这个值只作为 MPP 内部记账用的名义值：
       H.264 的 bps 字段仍参与 bit_per_frame 计算，不能为 0 */
    s->bitrate_kbps = bitrate_kbps > 0 ? bitrate_kbps
                                       : (int)((long long)w * h * 30 * 90 / 100 / 1000 / 1000) * 1000;
    if (s->bitrate_kbps < 12000) s->bitrate_kbps = 12000;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->wake, NULL);
    s->running = 1;
    if (pthread_create(&s->tid, NULL, annot_task, s) != 0) {
        pthread_mutex_destroy(&s->lock);
        pthread_cond_destroy(&s->wake);
        free(s);
        return NULL;
    }
    LOG_INFO("video_record: 标注录像线程启动（%dx%d, FIXQP qp=%d, 名义 %d kbps）",
             w, h, ANNOT_FIX_QP, s->bitrate_kbps);
    return s;
}

int video_record_push(video_record_t *s, const void *nv12, int w, int h,
                   const video_record_box_t *boxes, int count)
{
    if (!s || !nv12 || w <= 0 || h <= 0) return -1;
    if (atomic_load((atomic_int *)&s->disabled)) return -1;
    size_t len = (size_t)w * h * 3 / 2;
    unsigned char *copy = malloc(len);
    if (!copy) return -1;
    memcpy(copy, nv12, len);          /* 数据由解码器在出队瞬间拷好，这里只是转交 */

    pthread_mutex_lock(&s->lock);
    if (!s->running) { pthread_mutex_unlock(&s->lock); free(copy); return -1; }
    if (s->count == ANNOT_QUEUE_CAP) {
        annot_frame_t old = s->q[s->head];
        s->q[s->head].nv12 = NULL;      /* 同上：交出所有权，避免 stop() 二次 free */
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

void video_record_stop(video_record_t *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->running = 0;
    pthread_cond_broadcast(&s->wake);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->tid, NULL);
    /* 只释放仍留在队列里的帧：已出队的槽位在出队时就被置 NULL 了 */
    for (int i = 0; i < ANNOT_QUEUE_CAP; i++) free(s->q[i].nv12);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->wake);
    free(s);
}

void video_record_stats(const video_record_t *s, unsigned long long *frames,
                     unsigned long long *dropped, unsigned *last_ms)
{
    if (!s) return;
    if (frames) *frames = atomic_load(&s->frames);
    if (dropped) *dropped = atomic_load(&s->dropped);
    if (last_ms) *last_ms = atomic_load(&s->last_ms);
}

unsigned long long video_record_frames(const video_record_t *s)
{
    return s ? (unsigned long long)atomic_load(&s->frames) : 0;
}

unsigned long long video_record_bytes(const video_record_t *s)
{
    return s ? (unsigned long long)atomic_load(&s->bytes_total) : 0;
}

int video_record_active(const video_record_t *s)
{
    return s && s->mp4;
}
