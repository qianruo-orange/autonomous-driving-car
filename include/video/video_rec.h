/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIDEO_REC_H
#define VIDEO_REC_H

#include <stddef.h>

/**
 * Recording and live H.264 output share the encoder owned by video_rec_task.
 * The task consumes frame-matched annotated NV12 buffers from the frame ring,
 * writes MP4 segments, and publishes H.264 to the streaming module. HTTP APIs
 * queue start/stop/split commands; file deletion is synchronized with segment
 * creation, publication and finalization.
 */

/* 录制状态快照（供 HTTP status 接口 / 前端轮询） */
typedef struct {
    int               recording;    /* 1 = 录制中 */
    char              file[128];    /* YYYYMMDD/filename, retained until finalization ends */
    unsigned long long start_ms;    /* 开始时刻（CLOCK_MONOTONIC ms） */
    unsigned long long frames;      /* 已写入帧数 */
    unsigned long long bytes;       /* 已写入数据字节数 */
    double             fps;         /* 实时平均帧率（最后若干帧） */
    int                split_pending;
    int                split_error;
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

/* Queue a keyframe-aligned file split while keeping the encoder/live stream.
   expected_file prevents a stale page from splitting a different recording.
   Returns 0 when queued, -1 when inactive/busy, -2 for a stale filename. */
int video_rec_split(const char *expected_file);

/* 结束当前录制段，让线程按最新配置续录下一段（不改变自动续录开关）。
   编码器随会话创建，码率系数等编码参数变更后须调用本函数才会生效；
   已手动停录时为空操作 */
void video_rec_cycle_session(void);

/* Delete a regular video relative to an already opened recordings day folder.
 * Serializes with creation/split/finalization and preserves the open file even
 * after recording has been disabled. Caller owns day_fd; no symlinks followed.
 * Returns 0 deleted, 1 active, 2 already absent, -1 error (errno is set). */
int video_rec_delete_file(int day_fd, const char *relative);

/* 拷贝当前录制状态 */
int video_rec_status(video_rec_status_t *out);

#endif /* VIDEO_REC_H */
