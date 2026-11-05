/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * 压缩录像线程：相机 JPEG → MPP 解码（NV12）→ MPP 硬件编码（H.264）→ .h264 文件。
 *
 * 与无损 AVI 录像完全解耦：采集路径只做一次 JPEG 拷贝入队，解码与编码都在本线程
 * 完成。队列有界、满时丢最旧帧，保证实时路径不被编码拖累。
 */
#define _GNU_SOURCE
#include "video/rec_encode.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <rockchip/rk_mpi.h>
#include <rockchip/mpp_enc_cfg.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_packet.h>

#include "core/log.h"

#define ENC_QUEUE_CAP   16          /* 在途帧上限：30 fps 下约 0.5 s */
#define ENC_SEGMENT_MB  512         /* 单文件上限，超过就换新文件 */

typedef struct {
    unsigned char *data;
    size_t         len;
    uint64_t       ts_ms;
} enc_frame_t;

struct rec_encode {
    pthread_t        tid;
    pthread_mutex_t  lock;
    pthread_cond_t   wake;
    int              running;

    enc_frame_t      q[ENC_QUEUE_CAP];
    int              head, tail, count;

    /* 输出 */
    char             dir[256];
    char             prefix[64];
    FILE            *fp;
    char             path[512];
    unsigned long long segment_bytes;

    /* 硬件 */
    MppCtx           dec_ctx;
    MppApi          *dec_mpi;
    MppCtx           enc_ctx;
    MppApi          *enc_mpi;
    MppBufferGroup   grp;
    MppBuffer        buf;
    int              w, h, fps, bitrate_kbps;
    int              enc_ready;
    int              dec_ready;

    /* 统计 */
    atomic_ullong    frames, dropped, bytes;
    atomic_uint      last_ms;
    atomic_int       disabled;      /* 硬件不具备所需能力时置位：线程退出，入队变空操作 */
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void stamp(char *out, size_t size)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(out, size, "%Y%m%d_%H%M%S", &tm);
}

static int open_segment(rec_encode_t *e)
{
    char when[32];
    stamp(when, sizeof(when));
    int n = snprintf(e->path, sizeof(e->path), "%s/%s_%s.h264", e->dir, e->prefix, when);
    if (n <= 0 || (size_t)n >= sizeof(e->path)) return -1;
    e->fp = fopen(e->path, "wb");
    if (!e->fp) {
        LOG_ERROR("rec_enc: 打开 %s 失败: %s", e->path, strerror(errno));
        return -1;
    }
    e->segment_bytes = 0;
    LOG_INFO("rec_enc: 压缩录像 %s（H.264，%d kbps）", e->path, e->bitrate_kbps);
    return 0;
}

static void close_segment(rec_encode_t *e)
{
    if (!e->fp) return;
    fflush(e->fp);
    fsync(fileno(e->fp));
    fclose(e->fp);
    e->fp = NULL;
}

/* ---- MPP 解码器 ---- */
static int decoder_open(rec_encode_t *e)
{
    MPP_RET rc = mpp_create(&e->dec_ctx, &e->dec_mpi);
    if (rc != MPP_OK) {
        LOG_ERROR("rec_enc: mpp_create 失败 rc=%d", rc);
        return -1;
    }
    rc = mpp_init(e->dec_ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
    if (rc != MPP_OK) {
        atomic_store(&e->disabled, 1);
        LOG_ERROR("rec_enc: 硬件编码不可用（MPP 解码器 rc=%d）；本次运行只保留无损 AVI 录像", rc);
        mpp_destroy(e->dec_ctx);
        e->dec_ctx = NULL;
        return -1;
    }
    /* MJPEG 解码必须走 simple 模式下的逐包语义，split 解析由解码器自行完成 */
    e->dec_ready = 1;
    return 0;
}

/* ---- MPP 编码器 ---- */
static int encoder_open(rec_encode_t *e, int w, int h)
{
    if (mpp_create(&e->enc_ctx, &e->enc_mpi) != MPP_OK) return -1;
    if (mpp_init(e->enc_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) return -1;

    MppEncCfg cfg = NULL;
    if (mpp_enc_cfg_init(&cfg) != MPP_OK) return -1;
    mpp_enc_cfg_set_s32(cfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(cfg, "prep:width", w);
    mpp_enc_cfg_set_s32(cfg, "prep:height", h);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", w);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", h);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(cfg, "prep:range", MPP_FRAME_RANGE_JPEG);

    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_VBR);
    mpp_enc_cfg_set_u32(cfg, "rc:max_reenc_times", 0);
    mpp_enc_cfg_set_u32(cfg, "rc:super_mode", 0);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", MPP_ENC_RC_DROP_FRM_DISABLED);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_thd", 20);
    mpp_enc_cfg_set_u32(cfg, "rc:drop_gap", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_target", e->bitrate_kbps * 1000);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_max", e->bitrate_kbps * 1000 * 17 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:bps_min", e->bitrate_kbps * 1000 * 15 / 16);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", e->fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", e->fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", e->fps * 2);

    /* H.264 编码器：baseline 不启用 B 帧，切片与熵编码用默认值 */
    mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);   /* high */
    mpp_enc_cfg_set_s32(cfg, "h264:level", 40);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);

    if (e->enc_mpi->control(e->enc_ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK) {
        mpp_enc_cfg_deinit(cfg);
        return -1;
    }
    mpp_enc_cfg_deinit(cfg);
    if (e->enc_mpi->control(e->enc_ctx, MPP_ENC_SET_HEADER_MODE,
                            (void *)(MppEncHeaderMode)MPP_ENC_HEADER_MODE_EACH_IDR) != MPP_OK)
        return -1;
    if (mpp_buffer_group_get_internal(&e->grp, MPP_BUFFER_TYPE_DRM) != MPP_OK) return -1;
    if (mpp_buffer_get(e->grp, &e->buf, (size_t)w * h * 3 / 2) != MPP_OK) return -1;

    e->w = w; e->h = h;
    e->enc_ready = 1;
    LOG_INFO("rec_enc: 硬件编码器就绪 %dx%d@%d fps, %d kbps", w, h, e->fps, e->bitrate_kbps);
    return 0;
}

/* 把解码出的 NV12 拷进编码缓冲（两边 stride 可能不同） */
static void copy_nv12(const MppFrame fr, MppBuffer dst, int w, int h)
{
    MppBuffer src = mpp_frame_get_buffer(fr);
    if (!src) return;
    const unsigned char *s = mpp_buffer_get_ptr(src);
    unsigned char *d = mpp_buffer_get_ptr(dst);
    RK_U32 hs = mpp_frame_get_hor_stride(fr);
    RK_U32 vs = mpp_frame_get_ver_stride(fr);
    for (int y = 0; y < h; y++)
        memcpy(d + (size_t)y * w, s + (size_t)y * hs, (size_t)w);
    const unsigned char *suv = s + (size_t)hs * vs;
    unsigned char *duv = d + (size_t)w * h;
    for (int y = 0; y < h / 2; y++)
        memcpy(duv + (size_t)y * w, suv + (size_t)y * hs, (size_t)w);
}

/* 取编码输出包并写文件；返回写入字节数 */
static int drain_packets(rec_encode_t *e)
{
    int written = 0;
    while (1) {
        MppPacket pkt = NULL;
        if (e->enc_mpi->encode_get_packet(e->enc_ctx, &pkt) != MPP_OK) break;
        if (!pkt) break;
        void *ptr = mpp_packet_get_pos(pkt);
        size_t len = mpp_packet_get_length(pkt);
        int eos = mpp_packet_get_eos(pkt);
        if (len && e->fp) {
            fwrite(ptr, 1, len, e->fp);
            e->segment_bytes += len;
            atomic_fetch_add(&e->bytes, len);
            written += (int)len;
        }
        mpp_packet_deinit(&pkt);
        if (eos) break;
    }
    return written;
}

static int encode_one(rec_encode_t *e, const unsigned char *jpeg, size_t len)
{
    if (!e->fp && open_segment(e) != 0) return -1;
    if (!e->dec_ready && decoder_open(e) != 0) return -1;

    MppPacket pkt = NULL;
    if (mpp_packet_init(&pkt, (void *)jpeg, len) != MPP_OK) {
        LOG_ERROR("rec_enc: packet init 失败");
        return -1;
    }
    MPP_RET ret = e->dec_mpi->decode_put_packet(e->dec_ctx, pkt);
    mpp_packet_deinit(&pkt);
    if (ret != MPP_OK) {
        LOG_ERROR("rec_enc: decode_put_packet 失败 ret=%d", ret);
        return -1;
    }

    MppFrame fr = NULL;
    for (int i = 0; i < 50; i++) {
        ret = e->dec_mpi->decode_get_frame(e->dec_ctx, &fr);
        if (ret == MPP_OK && fr) break;
        if (mpp_frame_get_eos(fr)) { mpp_frame_deinit(&fr); fr = NULL; }
        usleep(1000);
        fr = NULL;
    }
    if (!fr) {
        LOG_ERROR("rec_enc: decode_get_frame 超时");
        return -1;
    }

    int w = (int)mpp_frame_get_width(fr), h = (int)mpp_frame_get_height(fr);
    if (!e->enc_ready && encoder_open(e, w, h) != 0) {
        mpp_frame_deinit(&fr);
        return -1;
    }

    copy_nv12(fr, e->buf, e->w, e->h);
    mpp_frame_deinit(&fr);

    MppFrame out = NULL;
    if (mpp_frame_init(&out) != MPP_OK) return -1;
    mpp_frame_set_width(out, e->w);
    mpp_frame_set_height(out, e->h);
    mpp_frame_set_hor_stride(out, e->w);
    mpp_frame_set_ver_stride(out, e->h);
    mpp_frame_set_fmt(out, MPP_FMT_YUV420SP);
    mpp_frame_set_pts(out, 0);
    mpp_frame_set_buffer(out, e->buf);
    ret = e->enc_mpi->encode_put_frame(e->enc_ctx, out);
    mpp_frame_deinit(&out);
    if (ret != MPP_OK) {
        LOG_ERROR("rec_enc: encode_put_frame 失败 ret=%d", ret);
        return -1;
    }

    int wrote = drain_packets(e);
    static unsigned long long encoded;
    if (!wrote && !(encoded++ % 60))
        LOG_ERROR("rec_enc: encode_get_packet 未产出数据（第 %llu 帧）", encoded);
    if (e->segment_bytes >= (unsigned long long)ENC_SEGMENT_MB * 1024 * 1024) {
        close_segment(e);
        open_segment(e);
    }
    return 0;
}

static void *encode_task(void *arg)
{
    rec_encode_t *e = arg;
    pthread_setname_np(pthread_self(), "rec_encode");
    for (;;) {
        enc_frame_t f = {0};
        pthread_mutex_lock(&e->lock);
        while (e->running && e->count == 0)
            pthread_cond_wait(&e->wake, &e->lock);
        if (!e->running && e->count == 0) {
            pthread_mutex_unlock(&e->lock);
            break;
        }
        f = e->q[e->head];
        e->head = (e->head + 1) % ENC_QUEUE_CAP;
        e->count--;
        pthread_mutex_unlock(&e->lock);

        uint64_t t0 = now_ms();
        if (atomic_load(&e->disabled)) { free(f.data); break; }
        int ok = encode_one(e, f.data, f.len);
        if (ok == 0) atomic_fetch_add(&e->frames, 1);
        else atomic_fetch_add(&e->dropped, 1);
        atomic_store(&e->last_ms, (unsigned)(now_ms() - t0));
        free(f.data);
        static uint64_t last_report;
        uint64_t nowr = now_ms();
        if (nowr - last_report >= 5000u) {
            last_report = nowr;
            if (e->fp) fflush(e->fp);
            LOG_INFO("rec_enc: 状态 ok=%llu fail=%llu bytes=%llu 段=%llu 队列=%d 耗时=%ums",
                     (unsigned long long)atomic_load(&e->frames),
                     (unsigned long long)atomic_load(&e->dropped),
                     (unsigned long long)atomic_load(&e->bytes),
                     (unsigned long long)e->segment_bytes, e->count,
                     atomic_load(&e->last_ms));
        }
    }
    /* 冲刷编码器尾部帧 */
    if (e->enc_ready) {
        MppFrame out = NULL;
        if (mpp_frame_init(&out) == MPP_OK) {
            mpp_frame_set_eos(out, 1);
            e->enc_mpi->encode_put_frame(e->enc_ctx, out);
            mpp_frame_deinit(&out);
        }
        drain_packets(e);
    }
    close_segment(e);
    if (e->buf) { mpp_buffer_put(e->buf); e->buf = NULL; }
    if (e->grp) { mpp_buffer_group_put(e->grp); e->grp = NULL; }
    if (e->enc_ctx) { mpp_destroy(e->enc_ctx); e->enc_ctx = NULL; }
    if (e->dec_ctx) { mpp_destroy(e->dec_ctx); e->dec_ctx = NULL; }
    LOG_INFO("rec_enc: 线程退出，已编码 %llu 帧",
             (unsigned long long)atomic_load(&e->frames));
    return NULL;
}

rec_encode_t *rec_encode_start(const char *dir, const char *prefix, int w, int h,
                               int fps, int bitrate_kbps)
{
    if (!dir || !prefix || w <= 0 || h <= 0) return NULL;
    rec_encode_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    snprintf(e->dir, sizeof(e->dir), "%s", dir);
    snprintf(e->prefix, sizeof(e->prefix), "%s", prefix);
    e->w = w; e->h = h;
    e->fps = fps > 0 ? fps : 30;
    /* 默认码率：720p 约 2.5 Mbps，按像素数线性缩放 */
    e->bitrate_kbps = bitrate_kbps > 0 ? bitrate_kbps
                                       : (int)((long long)w * h * 30 / 1000 / 8 / 1000) * 1000;
    if (e->bitrate_kbps < 500) e->bitrate_kbps = 500;
    pthread_mutex_init(&e->lock, NULL);
    pthread_cond_init(&e->wake, NULL);
    e->running = 1;
    if (pthread_create(&e->tid, NULL, encode_task, e) != 0) {
        pthread_mutex_destroy(&e->lock);
        pthread_cond_destroy(&e->wake);
        free(e);
        return NULL;
    }
    LOG_INFO("rec_enc: 压缩录像线程启动，目标 %d kbps", e->bitrate_kbps);
    return e;
}

void rec_encode_push(rec_encode_t *e, const unsigned char *jpeg, size_t len, uint64_t ts_ms)
{
    if (!e || !jpeg || len < 4) return;
    if (jpeg[0] != 0xff || jpeg[1] != 0xd8) return;
    unsigned char *copy = malloc(len);
    if (!copy) return;
    memcpy(copy, jpeg, len);

    if (atomic_load((atomic_int *)&e->disabled)) { free(copy); return; }
    pthread_mutex_lock(&e->lock);
    if (!e->running) {
        pthread_mutex_unlock(&e->lock);
        free(copy);
        return;
    }
    if (e->count == ENC_QUEUE_CAP) {          /* 满：丢最旧帧，保新 */
        enc_frame_t old = e->q[e->head];
        e->head = (e->head + 1) % ENC_QUEUE_CAP;
        e->count--;
        atomic_fetch_add(&e->dropped, 1);
        pthread_mutex_unlock(&e->lock);
        free(old.data);
        pthread_mutex_lock(&e->lock);
    }
    e->q[e->tail].data = copy;
    e->q[e->tail].len = len;
    e->q[e->tail].ts_ms = ts_ms;
    e->tail = (e->tail + 1) % ENC_QUEUE_CAP;
    e->count++;
    pthread_cond_signal(&e->wake);
    pthread_mutex_unlock(&e->lock);
}

void rec_encode_stop(rec_encode_t *e)
{
    if (!e) return;
    pthread_mutex_lock(&e->lock);
    e->running = 0;
    pthread_cond_broadcast(&e->wake);
    pthread_mutex_unlock(&e->lock);
    pthread_join(e->tid, NULL);
    for (int i = 0; i < ENC_QUEUE_CAP; i++) free(e->q[i].data);
    pthread_mutex_destroy(&e->lock);
    pthread_cond_destroy(&e->wake);
    free(e);
}

void rec_encode_stats(const rec_encode_t *e, unsigned long long *frames,
                      unsigned long long *dropped, unsigned long long *bytes,
                      unsigned *last_encode_ms)
{
    if (!e) return;
    if (frames) *frames = atomic_load(&e->frames);
    if (dropped) *dropped = atomic_load(&e->dropped);
    if (bytes) *bytes = atomic_load(&e->bytes);
    if (last_encode_ms) *last_encode_ms = atomic_load(&e->last_ms);
}
