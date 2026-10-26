/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_nettest.c — 网络测试 API（ping 可达性 + 网速测试）。
 *
 * 异步模型：POST /api/nettest 立即返回 test_id，后台线程执行测试；
 * GET /api/nettest/result?id=N 轮询结果。最多同时 4 个测试。
 */

#include "http/http_internal.h"
#include <pthread.h>
#include <time.h>

#define NT_MAX_TESTS    4
#define NT_CMD_BUF      512
#define NT_OUT_BUF      2048
#define NT_ERR_BUF      512

typedef struct {
    int      in_use;
    int      test_id;
    char     status[16];   /* pending / running / done */
    char     type[16];     /* ping / speed */
    char     target[256];
    char     result[NT_OUT_BUF];
    char     error[NT_ERR_BUF];
} nettest_t;

static nettest_t          nt_tests[NT_MAX_TESTS];
static pthread_mutex_t    nt_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int        nt_next_id = 1;

static int nt_alloc(int test_id)
{
    for (int i = 0; i < NT_MAX_TESTS; i++) {
        if (!nt_tests[i].in_use) {
            memset(&nt_tests[i], 0, sizeof(nt_tests[i]));
            nt_tests[i].in_use  = 1;
            nt_tests[i].test_id = test_id;
            return i;
        }
    }
    return -1;
}

/* ---- ping 后台线程：解析丢包率 ---- */
static void *nt_run_ping(void *arg)
{
    int id = *(int *)arg;
    free(arg);

    pthread_mutex_lock(&nt_mutex);
    int idx = -1;
    for (int i = 0; i < NT_MAX_TESTS; i++)
        if (nt_tests[i].in_use && nt_tests[i].test_id == id) { idx = i; break; }
    if (idx < 0) { pthread_mutex_unlock(&nt_mutex); return NULL; }
    strcpy(nt_tests[idx].status, "running");
    char target[256];
    strncpy(target, nt_tests[idx].target, sizeof(target) - 1);
    target[sizeof(target) - 1] = '\0';
    pthread_mutex_unlock(&nt_mutex);

    char cmd[NT_CMD_BUF];
    snprintf(cmd, sizeof(cmd), "ping -c 4 -W 3 '%.200s' 2>&1", target);

    FILE *fp = popen(cmd, "r");
    char output[1024];
    size_t total = 0;
    if (fp) {
        while (total < sizeof(output) - 1) {
            size_t r = fread(output + total, 1, sizeof(output) - 1 - total, fp);
            if (r == 0) break;
            total += r;
        }
        output[total] = '\0';
        int ec = pclose(fp);

        pthread_mutex_lock(&nt_mutex);
        if (idx >= 0 && nt_tests[idx].in_use && nt_tests[idx].test_id == id) {
            if (ec == 0) {
                /* 解析 packet loss: "X% packet loss" */
                int loss = -1;
                const char *lp = strstr(output, "% packet loss");
                if (lp) {
                    const char *s = lp - 1;
                    while (s > output && *s >= '0' && *s <= '9') s--;
                    loss = atoi(s + 1);
                }
                /* 解析 rtt: "rtt min/avg/max/mdev = X/X/X/X ms" */
                double rtt_min = 0, rtt_avg = 0, rtt_max = 0;
                const char *rp = strstr(output, "rtt min/avg/max/mdev");
                if (!rp) rp = strstr(output, "round-trip min/avg/max");
                if (rp) {
                    const char *eq = strchr(rp, '=');
                    if (eq)
                        sscanf(eq, "= %lf/%lf/%lf", &rtt_min, &rtt_avg, &rtt_max);
                }
                if (loss >= 0) {
                    snprintf(nt_tests[idx].result, NT_OUT_BUF,
                        "{\"reachable\":true,\"packet_loss\":%d,"
                        "\"rtt_min\":%.2f,\"rtt_avg\":%.2f,\"rtt_max\":%.2f}",
                        loss, rtt_min, rtt_avg, rtt_max);
                } else {
                    snprintf(nt_tests[idx].result, NT_OUT_BUF,
                        "{\"reachable\":true,\"packet_loss\":-1}");
                }
                strcpy(nt_tests[idx].status, "done");
            } else {
                /* ping 失败：提取最后一行有意义的错误信息 */
                const char *last_nl = strrchr(output, '\n');
                const char *err_line = output;
                if (last_nl && last_nl > output) {
                    const char *prev = last_nl - 1;
                    while (prev > output && *prev != '\n') prev--;
                    if (*prev == '\n') err_line = prev + 1;
                }
                /* 清理换行和引号 */
                char clean[256];
                strncpy(clean, err_line, sizeof(clean) - 1);
                clean[sizeof(clean) - 1] = '\0';
                for (char *p = clean; *p; p++)
                    if (*p == '"' || *p == '\n' || *p == '\r') *p = ' ';
                snprintf(nt_tests[idx].result, NT_OUT_BUF,
                    "{\"reachable\":false}");
                snprintf(nt_tests[idx].error, NT_ERR_BUF, "\"%s\"", clean);
                strcpy(nt_tests[idx].status, "done");
            }
        }
        pthread_mutex_unlock(&nt_mutex);
    } else {
        pthread_mutex_lock(&nt_mutex);
        if (idx >= 0 && nt_tests[idx].in_use && nt_tests[idx].test_id == id) {
            snprintf(nt_tests[idx].error, NT_ERR_BUF, "\"无法执行 ping 命令\"");
            strcpy(nt_tests[idx].status, "done");
        }
        pthread_mutex_unlock(&nt_mutex);
    }
    return NULL;
}

/* ---- 网速测试后台线程：curl 下载测速 ---- */
static void *nt_run_speed(void *arg)
{
    int id = *(int *)arg;
    free(arg);

    pthread_mutex_lock(&nt_mutex);
    int idx = -1;
    for (int i = 0; i < NT_MAX_TESTS; i++)
        if (nt_tests[i].in_use && nt_tests[i].test_id == id) { idx = i; break; }
    if (idx < 0) { pthread_mutex_unlock(&nt_mutex); return NULL; }
    strcpy(nt_tests[idx].status, "running");
    char target[256];
    strncpy(target, nt_tests[idx].target, sizeof(target) - 1);
    target[sizeof(target) - 1] = '\0';
    pthread_mutex_unlock(&nt_mutex);

    char cmd[NT_CMD_BUF];
    snprintf(cmd, sizeof(cmd),
        "curl -o /dev/null -s -w '%%{speed_download} %%{size_download} %%{time_total}' "
        "--noproxy '*' --max-time 10 --connect-timeout 5 '%.200s' 2>&1", target);

    FILE *fp = popen(cmd, "r");
    char output[256];
    size_t total = 0;
    if (fp) {
        while (total < sizeof(output) - 1) {
            size_t r = fread(output + total, 1, sizeof(output) - 1 - total, fp);
            if (r == 0) break;
            total += r;
        }
        output[total] = '\0';
        int ec = pclose(fp);

        pthread_mutex_lock(&nt_mutex);
        if (idx >= 0 && nt_tests[idx].in_use && nt_tests[idx].test_id == id) {
            if (ec == 0 && total > 0) {
                double speed = 0, size = 0, tm = 0;
                if (sscanf(output, "%lf %lf %lf", &speed, &size, &tm) == 3 && speed > 0) {
                    double mbps = (speed * 8.0) / 1000000.0;
                    snprintf(nt_tests[idx].result, NT_OUT_BUF,
                        "{\"speed_bps\":%.0f,\"speed_mbps\":%.2f,"
                        "\"size_bytes\":%.0f,\"time_sec\":%.2f}",
                        speed, mbps, size, tm);
                    strcpy(nt_tests[idx].status, "done");
                } else {
                    snprintf(nt_tests[idx].error, NT_ERR_BUF,
                        "\"无法解析测速结果\"");
                    strcpy(nt_tests[idx].status, "done");
                }
            } else {
                snprintf(nt_tests[idx].error, NT_ERR_BUF,
                    "\"下载失败，请检查网络连接\"");
                strcpy(nt_tests[idx].status, "done");
            }
        }
        pthread_mutex_unlock(&nt_mutex);
    } else {
        pthread_mutex_lock(&nt_mutex);
        if (idx >= 0 && nt_tests[idx].in_use && nt_tests[idx].test_id == id) {
            snprintf(nt_tests[idx].error, NT_ERR_BUF, "\"无法执行 curl 命令\"");
            strcpy(nt_tests[idx].status, "done");
        }
        pthread_mutex_unlock(&nt_mutex);
    }
    return NULL;
}

/* ---- POST /api/nettest — 启动测试 ---- */
void http_nettest_post(app_ctx_t *app, int fd, const char *method,
                       const char *uri, const char *req)
{
    (void)app; (void)uri;
    if (strcmp(method, "POST") != 0) {
        http_err(fd, 405, "Method Not Allowed", NULL);
        return;
    }

    const char *body = req;
    http_form_field_t fields[8];
    int nf = http_form_parse(body, fields, 8);

    const char *type   = http_form_find(fields, nf, "type");
    const char *target = http_form_find(fields, nf, "target");

    if (!type || !target || (type[0] == '\0') || (target[0] == '\0')) {
        http_err(fd, 400, "Bad Request", "{\"error\":\"missing type or target\"}");
        return;
    }

    /* 校验类型 */
    int is_ping = (strcmp(type, "ping") == 0);
    int is_speed = (strcmp(type, "speed") == 0);
    if (!is_ping && !is_speed) {
        http_err(fd, 400, "Bad Request", "{\"error\":\"invalid type\"}");
        return;
    }

    /* 校验 target：禁止 shell 注入字符 */
    if (strchr(target, ';') || strchr(target, '|') ||
        strchr(target, '&') || strchr(target, '$') ||
        strchr(target, '`') || strchr(target, '\n') ||
        strlen(target) > 200) {
        http_err(fd, 400, "Bad Request", "{\"error\":\"invalid target\"}");
        return;
    }

    pthread_mutex_lock(&nt_mutex);
    int test_id = atomic_fetch_add(&nt_next_id, 1);
    int idx = nt_alloc(test_id);
    if (idx < 0) {
        pthread_mutex_unlock(&nt_mutex);
        http_err(fd, 503, "Service Unavailable",
                 "{\"error\":\"too many concurrent tests\"}");
        return;
    }
    strncpy(nt_tests[idx].type, type, sizeof(nt_tests[idx].type) - 1);
    strncpy(nt_tests[idx].target, target, sizeof(nt_tests[idx].target) - 1);
    strcpy(nt_tests[idx].status, "pending");
    pthread_mutex_unlock(&nt_mutex);

    /* 启动后台线程 */
    int *id_arg = malloc(sizeof(int));
    *id_arg = test_id;
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (is_ping)
        pthread_create(&tid, &attr, nt_run_ping, id_arg);
    else
        pthread_create(&tid, &attr, nt_run_speed, id_arg);
    pthread_attr_destroy(&attr);

    char json[128];
    int off = 0;
    JSON_ADD(json, off, "{\"test_id\":%d,\"status\":\"pending\"}", test_id);
    http_ok_json(fd, json, (size_t)off);
}

/* ---- GET /api/nettest/result?id=N — 查询结果 ---- */
void http_nettest_result(app_ctx_t *app, int fd, const char *method,
                         const char *uri, const char *req)
{
    (void)app; (void)method;
    const char *qs = strchr(uri, '?');
    if (!qs) {
        http_err(fd, 400, "Bad Request", "{\"error\":\"missing id\"}");
        return;
    }
    int test_id = 0;
    const char *p = strstr(qs, "id=");
    if (p) test_id = atoi(p + 3);
    if (test_id <= 0) {
        http_err(fd, 400, "Bad Request", "{\"error\":\"invalid id\"}");
        return;
    }

    pthread_mutex_lock(&nt_mutex);
    int idx = -1;
    for (int i = 0; i < NT_MAX_TESTS; i++)
        if (nt_tests[i].in_use && nt_tests[i].test_id == test_id) { idx = i; break; }

    char json[2560];
    int off = 0;
    if (idx < 0) {
        pthread_mutex_unlock(&nt_mutex);
        snprintf(json, sizeof(json), "{\"error\":\"test not found\"}");
        http_ok_json(fd, json, strlen(json));
        return;
    }

    /* 构建基础字段 */
    off = snprintf(json, sizeof(json),
        "{\"test_id\":%d,\"status\":\"%s\",\"type\":\"%s\",\"target\":\"%s\"",
        test_id, nt_tests[idx].status, nt_tests[idx].type, nt_tests[idx].target);

    /* result: 原始 JSON 对象或 null */
    if (nt_tests[idx].result[0])
        off += snprintf(json + off, sizeof(json) - off,
            ",\"result\":%s", nt_tests[idx].result);
    else
        off += snprintf(json + off, sizeof(json) - off, ",\"result\":null");

    /* error: 带引号的字符串或 null */
    if (nt_tests[idx].error[0])
        off += snprintf(json + off, sizeof(json) - off,
            ",\"error\":%s", nt_tests[idx].error);
    else
        off += snprintf(json + off, sizeof(json) - off, ",\"error\":null");

    off += snprintf(json + off, sizeof(json) - off, "}");
    pthread_mutex_unlock(&nt_mutex);

    http_ok_json(fd, json, (size_t)off);
}
