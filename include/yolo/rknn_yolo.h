/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RKNN_YOLO_H
#define RKNN_YOLO_H

#include <stddef.h>

#include "yolo/yolo_types.h"

/**
 * yolo/rknn_yolo.h — RKNN + YOLO26 检测框架（主模块：模型管理 + 多线程推理池）。
 *
 * 模块组成（见 source/yolo/ 下各文件）：
 *  - yolo_image.c      图像处理（未压缩像素转换 / 缩放）
 *  - yolo_cv.cpp       依赖 OpenCV 的预处理（letterbox 缩放、线程数收敛）
 *  - yolo_classes.c    类别名表加载（config/coco.names，回退内置 COCO 80 类）
 *  - yolo_postprocess.c decoded YOLO26 detection filtering and NMS
 *  - rknn_yolo.c       模型加载、推理线程池（每线程独立 rknn context，并行推理）、
 *                      任务队列、结果队列（NMS 后处理结果：目标名称 + 置信度 +
 *                      标注框四点坐标）、结果快照
 *
 * 多线程推理：按实际编码/渲染帧率和帧龄试探比较活跃 worker 数，
 * 按实际 NPU 核数均匀分配，独立 rknn context 并行推理。
 * 推理后数据流分两路：rgb 写槽（画框显示链路），
 * NMS 结果入结果队列；composer 消费队列最新结果（乱序完成的旧结果丢弃），
 * 按 seq 严格递增做 EMA 平滑 + 渲染，每个视频帧都会产出标注画面。
 *
 * 热重载：rknn_yolo_reload() 停池 → 重读配置（模型/标签/阈值等）→ 重建。
 *
 * 必要流程：YOLO 推理不可停用。无模型 / NPU 驱动未加载 / 推理失败 → 直接报错：
 * 启动路径整体失败退出；运行期热重载失败由 HTTP 接口报告，画框流返回 503。
 * 所有失败路径只记日志不崩溃。
 */

/* 初始化：读配置加载模型并创建推理线程池；返回 0 成功。
   模型缺失/NPU 不可用返回 -1（无 YOLO 直接报错，服务启动失败） */
int rknn_yolo_init(void *arg);
void rknn_yolo_destroy(void *arg);
int rknn_yolo_enabled(void);
/* Planned pool replacement pauses annotated frames without a runtime fault. */
int rknn_yolo_reloading(void);
/* RK3576 固定双核 NPU（不再按平台探测）。 */
int npu_core_count(void);
/* 固定返回 "rk3576"；保留该接口仅为兼容旧模型的路径迁移。 */
const char *npu_platform_name(void);
/* Conversion, runtime defaults and uploads share config/yolo26.rknn. */
void rknn_yolo_default_model_path(char *out, size_t size);
/* Probe an uploaded model before replacing the active file. Returns 0 for one
 * input/one output, -1 for an unreadable/incompatible model. Leaves the running
 * pool untouched and releases the temporary SDK context before returning. */
int rknn_yolo_validate_model(const char *path);
/* 当前自适应启用的推理 worker 数；推理池不可用时返回 0。 */
int rknn_yolo_active_workers(void);
/* Read-only backend selection; the image thread owns the decoder. */
int rknn_yolo_hardware_decoder(void);

/* 读取最近若干帧的检测结果快照（按 seq 严格递增返回，最旧在前）。
   after 为客户端已持有的最大 seq，只有 seq > after 的结果才会返回，便于增量拉取。
   返回写入 out 的条数（0 表示暂无新结果或 YOLO 未运行）。
   前端 MJPEG 透传链路用它把检测坐标与原始画面按 seq 精确对齐。 */
int rknn_yolo_results_since(unsigned long long after, yolo_result_t *out, int max);

/* 类别名（只读，进程生命周期内有效）；越界返回 "object"。 */
const char *rknn_yolo_class_name(int cls);

/* 热重载推理池（停止 → 重读配置 → 重建）；返回 0 = 新池可用，-1 = 未运行/降级 */
int rknn_yolo_reload(void);

/* YOLO 工作线程（main 模块框架 task）：采集帧 → 解码 → 投递推理队列 */
void *rknn_yolo_task(void *arg);

#endif /* RKNN_YOLO_H */
