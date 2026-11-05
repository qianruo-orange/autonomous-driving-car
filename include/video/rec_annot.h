/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef REC_ANNOT_H
#define REC_ANNOT_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/rec_annot.h —— 带检测框的标注录像（H.264/MP4）。
 *
 * 取推理路径已经解出的 NV12 dma-buf（零额外解码），在 Y 平面直接画框，
 * 交给 MPP 硬件编码，再封装成 MP4。与无损 AVI 录像互不影响：
 * AVI 仍写相机原始帧，本模块只是额外产出一份带框的压缩视频。
 *
 * 队列有界、满时丢最旧帧，编码线程不会阻塞推理。
 */

typedef struct rec_annot rec_annot_t;

/* 框（原图坐标，与 yolo_result_t 一致） */
typedef struct {
    float x1, y1, x2, y2;
    int   cls;
} rec_annot_box_t;

/* 启动线程并创建第一个 MP4。bitrate_kbps 为 0 时按分辨率估算。 */
rec_annot_t *rec_annot_start(const char *dir, const char *prefix, int w, int h, int fps,
                             int bitrate_kbps, char *name_out, size_t name_size);

/* 提交一帧：NV12 dma-buf fd + 该帧的检测框。内部立即拷贝，fd 可马上归还。
   返回 0 成功（或队列丢弃），-1 已停止。 */
int rec_annot_push(rec_annot_t *s, const void *nv12, int w, int h,
                   const rec_annot_box_t *boxes, int count);

/* 停止线程、冲刷编码器、收尾 MP4 并释放。 */
void rec_annot_stop(rec_annot_t *s);

/* 统计：已编码帧、丢弃帧、最近一帧编码耗时（ms） */
void rec_annot_stats(const rec_annot_t *s, unsigned long long *frames,
                     unsigned long long *dropped, unsigned *last_ms);

/* 会话统计：已编码帧数与累计字节数（供 /api/rec/status）。 */
unsigned long long rec_annot_frames(const rec_annot_t *s);
unsigned long long rec_annot_bytes(const rec_annot_t *s);

/* 当前文件是否已打开（会话启动后第一帧才会真正建文件）。 */
int rec_annot_active(const rec_annot_t *s);

#endif /* REC_ANNOT_H */
