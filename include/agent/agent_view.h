/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AGENT_VIEW_H
#define AGENT_VIEW_H

/**
 * agent_view.h — 智能体的"视觉"通道。
 *
 * agent_hw.c 负责"能做"（凭证、包络、租约、执行器），本模块负责"能看"：
 * 把 NPU 检测结果与跟踪状态压成一份带语义的观测，并把当前相机原始 JPEG
 * 原样交给调用方。两者都走同一枚能力凭证（scope=observe），没有凭证拿不到
 * 任何一个像素。
 *
 * 设计取向：观测是只读的、无副作用的、可在任意频率调用的；它不写盘、不改
 * 控制状态、不持有锁跨请求。图像不做二次编码（相机本身输出 MJPEG），因此
 * 单帧开销只有一次 memcpy。
 */

#include <stddef.h>

/* 把 perception JSON 对象追加到 out（不含外层 key），返回写入后的偏移；
   失败返回 -1。size 不足时截断为 -1，不做部分写入。 */
int agent_perception_json(char *out, size_t size, int off);

/* HTTP 处理函数。frame 支持 ?b64=1（JSON 内嵌 base64，便于单次工具调用返回）
   与 ?max_bytes=N（超过则 413，避免把 4 MB 的帧塞给上游模型）。 */
void http_agent_perception(struct app_ctx_t *app, int fd, const char *method,
                           const char *uri, const char *req);
void http_agent_frame(struct app_ctx_t *app, int fd, const char *method,
                      const char *uri, const char *req);

#endif
