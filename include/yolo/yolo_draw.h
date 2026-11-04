/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef YOLO_DRAW_H
#define YOLO_DRAW_H

#include <stddef.h>

#include "yolo/yolo_types.h"

/**
 * yolo/yolo_draw.h — 检测框渲染模块：RGB 画框与类别标签。
 * 纯函数，无全局状态，可被任意线程并发调用。
 */

/* 加载类别名文件（官方 COCO 格式，每行一个类名，忽略空行）。
   文件缺失或为空时回退内置 COCO 80 类。返回加载到的类别数 */
int yolo_classes_load(const char *path, yolo_classes_t *out);

/* Draw boxes and labels in the caller-owned RGB frame. Returns 0 on success. */
int yolo_render_annotated(unsigned char *rgb, int w, int h,
                          const yolo_result_t *res,
                          const yolo_classes_t *classes, int tracked_index,
                          const unsigned long long *object_ids);

#endif /* YOLO_DRAW_H */
