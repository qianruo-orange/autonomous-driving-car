/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AGENT_HW_H
#define AGENT_HW_H

/**
 * agent_hw.h — 外部智能体（LLM / 多智能体）的硬件能力层。
 *
 * 设计要点：智能体拿到的不是 shell，也不是 root Basic Auth，而是一枚
 * 短期、带作用域、带包络、带里程预算的能力凭证；凭证只存在内存里，
 * 服务重启即失效；所有动作由人（root）显式 arm 之后才可能落到执行器。
 */

#include "core/common.h"

#define AGENT_SCOPE_OBSERVE 0x1u
#define AGENT_SCOPE_MOTION  0x2u

/* 凭证长度（十六进制字符）与同时存活的最大数量 */
#define AGENT_TOKEN_HEX 32
#define AGENT_TOKENS    8

/* 审计与运行表象 */
#define AGENT_DIR        "recordings/agent"
/* 服务账号：凭证落盘后由它读取，也是 codex 桥接降权后的目标用户 */
#define AGENT_SERVICE_GROUP "qianruo"

int agent_hw_init(void *app);
void agent_hw_destroy(void *app);
/* 供 agent_view.c 复用同一套凭证校验（header 或 bearer）。通过返回 0。 */
int agent_cap_authorize(const char *req, unsigned need);
/* 供其它模块在关机路径上强制撤销全部凭证 */
void agent_hw_revoke_all(const char *reason);

void http_agent_token(app_ctx_t *, int, const char *, const char *, const char *);
void http_agent_revoke(app_ctx_t *, int, const char *, const char *, const char *);
void http_agent_arm(app_ctx_t *, int, const char *, const char *, const char *);
void http_agent_disarm(app_ctx_t *, int, const char *, const char *, const char *);
void http_agent_motion(app_ctx_t *, int, const char *, const char *, const char *);
void http_agent_stop(app_ctx_t *, int, const char *, const char *, const char *);
/* 只放弃租约、保留 profile：一条计划正常结束时用它，下一条计划才能直接接上 */
void http_agent_release(app_ctx_t *, int, const char *, const char *, const char *);
void http_agent_estop(app_ctx_t *, int, const char *, const char *, const char *);
void http_agent_observe(app_ctx_t *, int, const char *, const char *, const char *);
void http_agent_state(app_ctx_t *, int, const char *, const char *, const char *);
void http_agent_audit(app_ctx_t *, int, const char *, const char *, const char *);

#endif
