/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LIVE_H264_H
#define LIVE_H264_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/live_h264.h —— 直播用 H.264 分发（MSE 播放）。
 *
 * 送进来的帧与标注录像同源：推理路径解好的 NV12 + 该帧的检测框。编码线程
 * 画框后用 VEPU510 硬件编成 H.264，再经 fmp4 打成 init segment + 每帧分片，
 * 放进一个有界环形缓冲；每个 HTTP 客户端持一个游标，从缓冲里取还没发过的
 * 分片。落后太多时丢掉旧分片而不是无限堆积。
 */

typedef struct {
    float x1, y1, x2, y2;
} live_box_t;

int  live_h264_start(int w, int h, int fps, int bitrate_kbps);
void live_h264_stop(void);

/* 提交一帧（NV12 dma-buf + 框）。内部立即拷贝，fd 可马上归还。 */
void live_h264_push(int nv12_fd, int w, int h, const live_box_t *boxes, int count);

/* 客户端游标：attach 返回非负游标，detach 释放。 */
int  live_h264_attach(void);
void live_h264_detach(int cursor);

/* 取下一段待发数据：成功返回 1 并给出指针与长度（指针在下一次取之前有效；
   返回 0 表示暂无新数据；返回 -1 表示游标落后太多，调用方应重新拉 init）。 */
int  live_h264_next(int cursor, const unsigned char **buf, size_t *len);

/* init segment（ftyp+moov），新客户端先发这一段。没有则返回 0。 */
int  live_h264_init(const unsigned char **buf, size_t *len);

#endif /* LIVE_H264_H */
