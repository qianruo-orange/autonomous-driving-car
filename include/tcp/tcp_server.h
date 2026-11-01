/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "core/common.h"
#define TCP_SERVICE_MAX 4
#define TCP_CLIENTS_PER_PORT 2
#define TCP_REPLY_CAPACITY 256
typedef struct tcp_ctx tcp_ctx_t;
#include "core/data_slot.h"
#define TCP_SLOT_CAPACITY 16
/* 所有接入、数据、断开事件都按顺序写入业务自己的接收槽。
 * TCP_RESET 表示槽溢出，连接已关闭，业务必须清除旧的半包状态。 */
typedef enum { TCP_CONNECTED, TCP_DATA, TCP_DISCONNECTED, TCP_RESET } tcp_data_kind_t;
typedef struct {
    tcp_data_kind_t kind;
    int port;
    uint64_t handle; /* opaque generation-tagged business connection handle */
    size_t length;
    unsigned char bytes[512];
} tcp_data_t;
typedef struct {
    data_slot_t queue;
    tcp_data_t items[TCP_SLOT_CAPACITY];
} tcp_slot_t;
static inline int tcp_slot_init(tcp_slot_t *s)
{
    return data_slot_init(&s->queue, s->items, sizeof(tcp_data_t), TCP_SLOT_CAPACITY);
}
/* Data always arrives through the slot. Optional poll drains it on the shared
 * TCP worker, outside transport locks; a business worker may drain it instead. */
typedef struct {
    tcp_slot_t *slot;
    int (*poll)(app_ctx_t *, void *user);
} tcp_component_t;
typedef struct {
    int service, fd, port, enabled, replace;
    uint32_t token;
    uint64_t handle;
} tcp_service_stage_t;
typedef struct {
    int running, listening, clients;
    unsigned long long sent_frames, dropped_frames, errors;
    char error[40];
} tcp_service_status_t;
/* 简单接入使用 component_register；业务注册端口并绑定自己的 slot。
 * 返回端口句柄，可广播；接收槽里的 handle 则只对应当次客户端连接。
 * 组件描述符、user 和 slot 在 TCP 线程退出前必须保持有效。
 * prepare/commit/cancel_register 仅用于需要先保存配置、再切换端口的场景；
 * 准备失败不改变现有连接，准备成功后 commit 不再做可能失败的 socket 操作。 */
/* Returns a business handle; zero means failure. Register after tcp_init. */
uint64_t tcp_component_register(int port, const tcp_component_t *, void *user);
int tcp_prepare_register(tcp_ctx_t *, uint64_t previous_handle, int enabled, int port,
                         const tcp_component_t *, void *user, tcp_service_stage_t *);
void tcp_commit_register(tcp_ctx_t *, tcp_service_stage_t *);
void tcp_cancel_register(tcp_ctx_t *, tcp_service_stage_t *);
/* Registration handle broadcasts; received handles address one live client. */
/* 业务只传句柄和原始数据；TCP 查找绑定的端口/连接并处理短写。
 * 单次最多 256 字节；繁忙的定向连接返回 ENOBUFS，陈旧连接不能复用 fd。 */
int tcp_send(uint64_t handle, const void *data, size_t length);
int tcp_component_unregister(uint64_t handle);
void tcp_service_get_status(tcp_ctx_t *, uint64_t handle, tcp_service_status_t *);
int tcp_ready(tcp_ctx_t *);
void tcp_wake(tcp_ctx_t *);
int tcp_init(void *);
void tcp_cleanup(void *);
void *tcp_task(void *);
