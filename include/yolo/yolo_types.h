/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef YOLO_TYPES_H
#define YOLO_TYPES_H

#include <stddef.h>

/* 一帧最多保留的检测框数 */
#define YOLO_MAX_DETS 32

/* 单条检测结果（坐标在原图坐标系，x1<x2, y1<y2） */
typedef struct {
    float x1, y1, x2, y2;
    float conf;
    int   cls;
} yolo_det_t;

/* 一帧的检测结果快照 */
typedef struct {
    int               count;
    unsigned long long seq;   /* 对应的视频采集帧序号 */
    /* 计时用：解码结束与后处理结束时刻（CLOCK_MONOTONIC 微秒）。
       渲染侧据此算"后处理完成 → 显示提交"的交接开销。 */
    unsigned long long decode_end_us;
    unsigned long long post_done_us;
    int               w, h;   /* 检测原图尺寸（解码后） */
    yolo_det_t        dets[YOLO_MAX_DETS];
} yolo_result_t;

/* 类别名表（官方 COCO 格式文件 config/coco.names，每行一个类名） */
#define YOLO_MAX_CLASSES     128
#define YOLO_CLASS_NAME_LEN  24
typedef struct {
    int  count;
    char names[YOLO_MAX_CLASSES][YOLO_CLASS_NAME_LEN];
} yolo_classes_t;

#endif /* YOLO_TYPES_H */
