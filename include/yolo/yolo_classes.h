/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef YOLO_CLASSES_H
#define YOLO_CLASSES_H

#include "yolo/yolo_types.h"

/**
 * yolo/yolo_classes.h — 类别名表加载（纯 C，无第三方依赖）。
 *
 * 原先与检测框渲染一起放在 yolo_draw.cpp 里；画框逻辑删除后，
 * 这份纯数据加载单独成模块，避免为了加载类名而拖入 OpenCV。
 */

/* 加载类别名文件（官方 COCO 格式，每行一个类名，忽略空行）。
   文件缺失或为空时回退内置 COCO 80 类。返回加载到的类别数 */
int yolo_classes_load(const char *path, yolo_classes_t *out);

#endif /* YOLO_CLASSES_H */
