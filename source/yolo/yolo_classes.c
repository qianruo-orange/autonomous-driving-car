/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * yolo_classes.c — 类别名表加载。
 *
 * 类别名来自 config/coco.names（官方 COCO 格式，每行一个类名）；
 * 文件缺失或为空时回退内置的 COCO 80 类，保证标签始终可读。
 * 纯函数，不持有全局状态。
 */

#include <stdio.h>
#include <string.h>

#include "yolo/yolo_classes.h"

/* ---- 内置回退类别名（COCO 80 类，官方顺序） ---- */
static const char *const kCoco80[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush",
};

int yolo_classes_load(const char *path, yolo_classes_t *out)
{
    memset(out, 0, sizeof(*out));
    int n = 0;
    FILE *fp = (path && path[0]) ? fopen(path, "r") : NULL;
    if (fp) {
        char line[YOLO_CLASS_NAME_LEN + 4];
        while (n < YOLO_MAX_CLASSES && fgets(line, sizeof(line), fp)) {
            char *s = line, *e = line + strlen(line);
            while (s < e && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
            char *t = e;
            while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r' || t[-1] == '\n')) t--;
            if (t == s) continue;   /* 空行 */
            size_t len = (size_t)(t - s);
            if (len >= YOLO_CLASS_NAME_LEN) len = YOLO_CLASS_NAME_LEN - 1;
            memcpy(out->names[n], s, len);
            out->names[n][len] = '\0';
            n++;
        }
        fclose(fp);
    }
    if (n == 0) {   /* 文件缺失或为空：回退内置 COCO 80 类 */
        int m = (int)(sizeof(kCoco80) / sizeof(kCoco80[0]));
        if (m > YOLO_MAX_CLASSES) m = YOLO_MAX_CLASSES;
        for (int i = 0; i < m; i++)
            snprintf(out->names[i], YOLO_CLASS_NAME_LEN, "%s", kCoco80[i]);
        n = m;
    }
    out->count = n;
    return n;
}
