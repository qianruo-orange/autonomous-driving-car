/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LIVE_H264_H
#define LIVE_H264_H

#include <stddef.h>

/**
 * video/live_h264.h —— 直播分发（fMP4 分片，供浏览器 MSE 播放）。
 *
 * 不做编码：标注录像线程把已经编好的 H.264 访问单元交给
 * live_h264_publish()，这里打成 init segment + 每帧分片放进有界环形缓冲，
 * 每个 HTTP 客户端持游标取用。单编码器双出口，避免两个 MPP 实例争用
 * 同一颗 VEPU510 而互相破坏码流。
 */

int  live_h264_start(int w, int h, int fps);
void live_h264_stop(void);

/* 送入一个 H.264 访问单元（Annex-B），由标注编码线程调用。 */
void live_h264_publish(const unsigned char *annexb, size_t len);

int  live_h264_attach(void);
void live_h264_detach(int cursor);
int  live_h264_next(int cursor, const unsigned char **buf, size_t *len);
int  live_h264_init(const unsigned char **buf, size_t *len);

#endif /* LIVE_H264_H */
