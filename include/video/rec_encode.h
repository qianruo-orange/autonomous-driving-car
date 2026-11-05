/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef REC_ENCODE_H
#define REC_ENCODE_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/rec_encode.h —— 压缩录像：独立编码线程把相机 JPEG 编成 H.264 保存。
 *
 * 与无损录像并行工作：无损路径把原始 JPEG 写进 AVI(MJPG)，本模块拿同一帧的
 * JPEG 副本，在**独立线程**里用硬件编解码器完成 MJPEG 解码 + H.264 编码，
 * 输出 Annex-B 裸流 .h264。采样率约 190 fps 的硬件吞吐远高于 30 fps 采集，
 * 因此编码线程不会拖慢采集与推理。
 *
 * 队列有界，满时丢最旧帧（保新），保证采集路径永远不被阻塞。
 */

typedef struct rec_encode rec_encode_t;

/* 启动编码线程。dir/prefix 决定输出文件名 <prefix>_YYYYMMDD_HHMMSS.h264。
   fps 写入 SPS 的 VUI，bitrate_kbps 目标码率（0 = 按分辨率的默认值）。
   失败返回 NULL（调用方继续无损录像即可）。 */
rec_encode_t *rec_encode_start(const char *dir, const char *prefix, int w, int h,
                               int fps, int bitrate_kbps);

/* 提交一帧相机原始 JPEG。内部拷贝后入队，队列满时丢最旧帧。
   模块未启动或已停止时空操作。 */
void rec_encode_push(rec_encode_t *enc, const unsigned char *jpeg, size_t len,
                     uint64_t ts_ms);

/* 停止线程、冲刷编码器、关闭当前文件。可重复调用。 */
void rec_encode_stop(rec_encode_t *enc);

/* 统计：已编码帧数、丢弃帧数、已写字节数、最近一次编码耗时（毫秒） */
void rec_encode_stats(const rec_encode_t *enc, unsigned long long *frames,
                      unsigned long long *dropped, unsigned long long *bytes,
                      unsigned *last_encode_ms);

#endif /* REC_ENCODE_H */
