/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef YOLO_POSTPROCESS_H
#define YOLO_POSTPROCESS_H

#include <stdint.h>

#include "yolo/rknn_api.h"
#include "yolo/yolo_types.h"

/**
 * yolo/yolo_postprocess.h — YOLO26 单输出模式后处理（官方 ultralytics rknn 导出格式）。
 * 纯函数，无全局状态，可被任意线程并发调用。
 */

/* 单输出模型后处理：框已解码（像素坐标）、分数已 sigmoid，仅做阈值过滤 + 类别内 NMS。
   @out_buf  输出的 float 数据（rknn_outputs_get 的 buf）
   @attrs    输出张量属性（决定布局 NCHW/NHWC 与尺寸）
   @n_output 必须为 1（官方 rknn 单输出），否则视为模型不匹配返回 0
   @lb_scale_x/@lb_scale_y/@lb_pad_x/@lb_pad_y 预处理 letterbox 几何：模型坐标经
              (coordinate - pad) / axis_scale 逆映射回原图坐标系
   @frame_w/@frame_h 原图尺寸（结果坐标所属坐标系）
   @nc_out   可选：输出推断出的类别数
   @return 检测数（已过滤、NMS 并映射到原图像素范围；无效几何返回 0） */
int yolo_postprocess(const float *const *out_buf, const rknn_tensor_attr *attrs,
                     uint32_t n_output,
                     float lb_scale_x, float lb_scale_y, int lb_pad_x, int lb_pad_y,
                     int frame_w, int frame_h,
                     yolo_det_t *dets, int max_dets,
                     float conf, float nms, int *nc_out);


/* YOLO26 one2one 双输出后处理（官方 rknn_model_zoo 截断方案，无 NMS）。
   输出 A 为类别分数 (1,80,A)（已 sigmoid），输出 B 为框 (1,4,A)，均为 INT8 量化、
   框为模型输入像素坐标 x1,y1,x2,y2。逐锚点取最大类，阈值过滤后按分数取前 max_dets。
   @return 检测数（已映射回原图坐标） */
int yolo_postprocess_two(const void *scores_buf, const rknn_tensor_attr *scores_attr,
                         const void *boxes_buf, const rknn_tensor_attr *boxes_attr,
                         float lb_scale_x, float lb_scale_y, int lb_pad_x, int lb_pad_y,
                         int frame_w, int frame_h,
                         yolo_det_t *dets, int max_dets, float conf, int *nc_out);

#endif /* YOLO_POSTPROCESS_H */
