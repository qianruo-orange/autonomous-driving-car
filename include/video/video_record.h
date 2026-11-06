/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIDEO_RECORD_H
#define VIDEO_RECORD_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/video_record.h —— 带检测框的标注录像（H.264/MP4）。
 *
 * 取推理路径已经解出的 NV12 dma-buf（零额外解码），在 Y/UV 平面画彩色框，
 * 交给 MPP 硬件编码（恒定 QP，不做码率控制），再封装成 MP4。这是录像的
 * 唯一产物，没有并行的第二路文件。
 *
 * 队列有界、满时丢最旧帧，编码线程不会阻塞推理。
 */

typedef struct video_record video_record_t;

/* 框（原图坐标，与 yolo_result_t 一致）。
   label 由调用方填好（如 "person 0.94"），为空时只画框不画标签。 */
typedef struct {
    float x1, y1, x2, y2;
    float conf;
    int   cls;
    char  label[24];
} video_record_box_t;

/* 启动线程并创建第一个 MP4。bitrate_kbps 在固定 QP 下不再影响画质，
   只作为 MPP 内部记账的名义码率；传 0 时按分辨率估算。 */
video_record_t *video_record_start(const char *dir, const char *prefix, int w, int h, int fps,
                             int bitrate_kbps, char *name_out, size_t name_size);

/* 提交一帧：NV12 内存 + 该帧的检测框。内部立即拷贝一份，调用方可以马上复用原缓冲。
   返回 0 成功（或队列丢弃），-1 已停止。 */
int video_record_push(video_record_t *s, const void *nv12, int w, int h,
                   const video_record_box_t *boxes, int count);

/* 停止线程、冲刷编码器、收尾 MP4 并释放。 */
void video_record_stop(video_record_t *s);

/* 统计：已编码帧、丢弃帧、最近一帧编码耗时（ms） */
void video_record_stats(const video_record_t *s, unsigned long long *frames,
                     unsigned long long *dropped, unsigned *last_ms);

/* 会话统计：已编码帧数与累计字节数（供 /api/rec/status）。 */
unsigned long long video_record_frames(const video_record_t *s);
unsigned long long video_record_bytes(const video_record_t *s);

/* 当前文件是否已打开（会话启动后第一帧才会真正建文件）。 */
int video_record_active(const video_record_t *s);

#endif /* VIDEO_RECORD_H */
