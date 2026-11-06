/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIDEO_REC_H
#define VIDEO_REC_H

#include <stddef.h>

#include "video/rec_annot.h"

/**
 * Recording sessions. The camera JPEG is decoded to NV12 (hardware JPEG
 * decoder), detection boxes are drawn on the Y plane, and the result is
 * encoded to H.264 (MPP, fixed QP) and muxed into MP4 — one product, one file
 * at a time. The recording thread drives the segment lifecycle (open at start,
 * rotate on day change or size cap, finalize with a moov box); HTTP APIs queue
 * start/stop/split commands; deletion is synchronized with segment creation,
 * publication and finalization.
 */

/* 录制状态快照（供 HTTP status 接口 / 前端轮询） */
typedef struct {
    int               recording;    /* 1 = 录制中 */
    char              file[128];    /* plain filename in recordings/videos/ */
    unsigned long long start_ms;    /* 开始时刻（CLOCK_MONOTONIC ms） */
    unsigned long long frames;      /* 已写入帧数 */
    unsigned long long bytes;       /* 已写入数据字节数 */
    double             fps;         /* 实时平均帧率（最后若干帧） */
    int                split_pending;
    int                split_error;
    char               encoder[32]; /* "h264-mp4" while recording, else "none" */
    /* 本段会话累计的跳过帧：录制跟不上采集 / 尺寸与配置不符 */
    unsigned long long skipped_cadence;
    unsigned long long skipped_size;
    /* 0=正常；1=建文件失败；2=写/定位失败；3=空间不足 */
    int                io_error;
    unsigned long long free_bytes;  /* 录像目录可用空间，取状态时读取 */
} video_rec_status_t;

struct app_config_t;

/* 模块生命周期（main 模块表调用）：init 建目录；task 为录制线程（空闲空转喂狗） */
int  video_rec_init(void *arg);
void video_rec_destroy(void *arg);
void *video_rec_task(void *arg);

/* HTTP 控制入口（Reactor 线程调用，线程安全）：
   @return 0 成功，-1 失败（已在录制 / 未在录制 / 参数错误） */
int video_rec_start(void);
int video_rec_stop(void);

/* Queue a frame-aligned file split. expected_file prevents a stale page from
   splitting a different recording.
   Returns 0 when queued, -1 when inactive/busy, -2 for a stale filename. */
int video_rec_split(const char *expected_file);

/* 结束当前录制段，让线程按最新配置续录下一段（不改变自动续录开关）。
   已手动停录时为空操作 */
void video_rec_cycle_session(void);

/* Delete a regular video by plain filename inside an already opened
 * recordings/videos directory. Serializes with creation/split/finalization and
 * preserves the open file even after recording has been disabled. Caller owns
 * directory_fd; no symlinks followed.
 * Returns 0 deleted, 1 active, 2 already absent, -1 error (errno is set). */
int video_rec_delete_file(int directory_fd, const char *name);

/* 拷贝当前录制状态 */
int video_rec_status(video_rec_status_t *out);


/* 标注录像：把一帧 NV12（dma-buf）连同检测框交给标注编码线程。
   boxes 为原图像素的框，label 由调用方填好（"person 0.94"）。
   未启用标注录像时为空操作。 */
void video_rec_annot_frame(const void *nv12, int w, int h,
                           const rec_annot_box_t *boxes, int count);

#endif /* VIDEO_REC_H */
