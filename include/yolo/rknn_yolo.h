/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RKNN_YOLO_H
#define RKNN_YOLO_H

#include <stddef.h>

#include "yolo/yolo_types.h"

/**
 * yolo/rknn_yolo.h — RKNN + YOLO26 检测框架（模型管理 + 解码/推理/渲染线程池）。
 *
 * 模块组成：
 *  - video/rga_preprocess.c  RGA 硬件预处理（NV12→RGB888 + letterbox）
 *  - yolo_classes.c          类别名表加载（config/coco.names，回退内置 COCO 80 类）
 *  - yolo_postprocess.c      YOLO26 输出解码、阈值过滤与类内 NMS
 *  - yolo_events.c           检测结果 TXT 事件记录
 *  - rknn_yolo.c             模型加载、解码/推理/渲染线程池、任务与结果队列、结果快照
 *
 * 数据流：yolo_task 按序 claim 帧环里的相机原始 JPEG → 2 个 decode 线程用
 * libjpeg-turbo 解成 NV12 dma-buf（另拷一份 NV12 给标注录像）→ 2 个 worker
 * 让 RGA 一趟完成色彩转换与缩放，交给 RKNN 推理（每线程独立 context，按 NPU
 * 核绑定）→ 后处理结果按值入结果队列 → render 线程排空只留最新，做运动学
 * 跟踪/EMA 并更新结果快照。检测框由浏览器 canvas 按 seq 叠加，服务端不生成
 * 标注画面；标注录像单独在 NV12 副本上画框后交给 MPP 编码。
 *
 * 热重载：rknn_yolo_reload() 停池 → 重读配置（模型/标签/阈值等）→ 重建。
 *
 * 必要流程：YOLO 推理不可停用。无模型 / NPU 驱动未加载 / 推理失败 → 报错：
 * 启动路径整体失败退出；运行期热重载失败由 HTTP 接口报告。
 */

/* 初始化：读配置加载模型并创建推理线程池；返回 0 成功。
   模型缺失/NPU 不可用返回 -1（无 YOLO 直接报错，服务启动失败） */
int rknn_yolo_init(void *arg);
void rknn_yolo_destroy(void *arg);
/* RK3576 固定双核 NPU（不再按平台探测）。 */
int npu_core_count(void);
/* 固定返回 "rk3576"；保留该接口仅为兼容旧模型的路径迁移。 */
const char *npu_platform_name(void);
/* Conversion, runtime defaults and uploads share config/yolo26.rknn. */
void rknn_yolo_default_model_path(char *out, size_t size);
/* Probe an uploaded model before replacing the active file. Returns 0 for one
 * input with one or two outputs, -1 for an unreadable/incompatible model. Leaves the running
 * pool untouched and releases the temporary SDK context before returning. */
int rknn_yolo_validate_model(const char *path);
/* 当前运行的推理 worker 数（RK3576 固定 2）；推理池不可用时返回 0。 */
int rknn_yolo_active_workers(void);
/* 解码器是否就绪，仅用于状态上报；libjpeg 实例由双解码线程各自持有。 */
int rknn_yolo_decoder_ready(void);

/* 读取最近若干帧的检测结果快照（按 seq 严格递增返回，最旧在前）。
   after 为客户端已持有的最大 seq，只有 seq > after 的结果才会返回，便于增量拉取。
   返回写入 out 的条数（0 表示暂无新结果或 YOLO 未运行）。
   前端 MJPEG 透传链路用它把检测坐标与原始画面按 seq 精确对齐。 */
int rknn_yolo_results_since(unsigned long long after, yolo_result_t *out, int max);

/* 取「最新一条」检测结果：每帧推理完成后覆盖写，只保留 seq 最大的那次。
   返回 1 = 已写入 out，0 = 还没有任何结果。
   需要最新一帧时必须用它，不能用 results_since(0, buf, N) 取末尾——那个函数
   是从最旧一条开始填的，且受 N 与环形深度 YOLO_HIST_N 的关系影响。 */
int rknn_yolo_latest(yolo_result_t *out);

/* 类别名（只读，进程生命周期内有效）；越界返回 "object"。 */
const char *rknn_yolo_class_name(int cls);

/* 热重载推理池（停止 → 重读配置 → 重建）；返回 0 = 新池可用，-1 = 未运行/降级 */
int rknn_yolo_reload(void);

/* YOLO 采集线程（main 模块框架 task）：按序 claim 帧环槽 → 投递解码队列 */
void *rknn_yolo_task(void *arg);

#endif /* RKNN_YOLO_H */
