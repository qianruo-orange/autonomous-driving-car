/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * yolo_cv.cpp — 依赖 OpenCV 的图像预处理。
 *
 * 只保留推理链路真正需要的部分：
 *  - yolo_image_init：把 OpenCV 线程数钉为 1。调用方（推理线程池）自己管并发，
 *    让 OpenCV 再开内部线程池会和推理抢核，导致抖动。
 *  - yolo_rgb_letterbox：等比例缩放 + 灰色填充居中，供 NPU 输入使用。
 *
 * 检测框/标签渲染（cv::rectangle / cv::putText）已删除：标注帧没有消费者，
 * 框改由浏览器端 canvas 按帧序号叠加一次（见 html/js/monitor.js overlayDraw）。
 */

#include <string.h>

extern "C" {
#include "yolo/yolo_image.h"
}

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

extern "C" {

int yolo_image_init(void)
{
    try { cv::setNumThreads(1); }
    catch (const cv::Exception &) { return -1; }
    return 0;
}

void yolo_rgb_letterbox(const unsigned char *src, int sw, int sh,
                        unsigned char *dst, int dw, int dh,
                        float *scale_x_out, float *scale_y_out, int *pad_x_out, int *pad_y_out)
{
    /* 等比例缩放并取整，scale 用实际尺寸比（与画布几何一致，逆映射才精确） */
    float s = (float)dw / sw < (float)dh / sh ? (float)dw / sw : (float)dh / sh;
    int nw = (int)(sw * s + 0.5f), nh = (int)(sh * s + 0.5f);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    int px = (dw - nw) / 2, py = (dh - nh) / 2;
    if (scale_x_out) *scale_x_out = (float)nw / (float)sw;
    if (scale_y_out) *scale_y_out = (float)nh / (float)sh;
    if (pad_x_out) *pad_x_out = px;
    if (pad_y_out) *pad_y_out = py;

    /* 整画布填灰边，内容 resize 进居中 ROI（NEON 加速，逐通道无关） */
    cv::Mat canvas(dh, dw, CV_8UC3, dst);
    canvas.setTo(cv::Scalar(114, 114, 114));
    cv::Mat s_(sh, sw, CV_8UC3, (void *)src);
    cv::Mat roi = canvas(cv::Rect(px, py, nw, nh));
    cv::resize(s_, roi, cv::Size(nw, nh), 0, 0, cv::INTER_LINEAR);
}

} /* extern "C" */
