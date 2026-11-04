/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef YOLO_IMAGE_H
#define YOLO_IMAGE_H

#include <stddef.h>

/**
 * yolo/yolo_image.h — 图像处理模块：未压缩像素格式转换与缩放。
 * 初始化后，图像转换可被任意线程并发调用。
 */

/* Call before starting image workers. Keep OpenCV work on its calling thread
 * so a reused library pool cannot bypass that thread's realtime scheduling. */
int yolo_image_init(void);

/* YUYV（V4L2）→ RGB24（BT.601），dst 需 w*h*3 字节 */
void yolo_yuyv_to_rgb(const unsigned char *src, int w, int h, unsigned char *dst);

/* Full-range BT.601 NV12 from the JPEG decoder; planes may include padding. */
void yolo_nv12_to_rgb_full(const unsigned char *y, const unsigned char *uv,
                            int stride, int w, int h, unsigned char *rgb);

/* RGB24 letterbox+padding（OpenCV SIMD）：等比例缩放到 dw×dh 内后居中，
   四周填 114 灰边（ultralytics 训练惯例）。输出实际缩放系数与左上偏移，
   供后处理把模型坐标逆映射回原图坐标系。X/Y 使用取整后实际尺寸比，
   避免非整比例分辨率在另一轴产生偏移。 */
void yolo_rgb_letterbox(const unsigned char *src, int sw, int sh,
                        unsigned char *dst, int dw, int dh,
                        float *scale_x_out, float *scale_y_out, int *pad_x_out, int *pad_y_out);

#endif /* YOLO_IMAGE_H */
