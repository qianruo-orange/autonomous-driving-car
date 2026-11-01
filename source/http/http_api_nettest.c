/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http/http.h"
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
#define NT_RESULT_TTL_SEC 60

typedef struct {
    int      in_use;
    int      test_id;
    char     status[16];   /* pending / running / done */
    char     type[16];     /* ping / speed */
    char     target[256];
    char     result[NT_OUT_BUF];
    char     error[NT_ERR_BUF];
    time_t   completed_at;
} nettest_t;

static nettest_t          nt_tests[NT_MAX_TESTS];
static pthread_mutex_t    nt_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int        nt_next_id = 1;

/* Caller holds nt_mutex while inspecting task slots. */
static int nt_find(int id)
{
    for (int i = 0; i < NT_MAX_TESTS; i++)
        if (nt_tests[i].in_use && nt_tests[i].test_id == id) return i;
    return -1;
}

static void nt_finish(int idx)
{
    strcpy(nt_tests[idx].status, "done");
    nt_tests[idx].completed_at = time(NULL);
}

static int nt_alloc(int test_id)
{
    for (int i = 0; i < NT_MAX_TESTS; i++) {
        if (!nt_tests[i].in_use ||
            (strcmp(nt_tests[i].status, "done") == 0 &&
             time(NULL) - nt_tests[i].completed_at >= NT_RESULT_TTL_SEC)) {
            memset(&nt_tests[i], 0, sizeof(nt_tests[i]));
            nt_tests[i].in_use  = 1;
            nt_tests[i].test_id = test_id;
            return i;
        }
    }
    return -1;
}

/* Copy task input while locked; workers execute commands outside the lock. */
static int nt_begin(int id, char *target, size_t size)
{
    pthread_mutex_lock(&nt_mutex);
    int idx = nt_find(id);
    if (idx >= 0) {
        strcpy(nt_tests[idx].status, "running");
        safe_strncpy(target, size, nt_tests[idx].target);
    }
    pthread_mutex_unlock(&nt_mutex);
    return idx;
}

/* Drain the pipe even after capture fills, so pclose cannot wait on a
 * child blocked writing output. All callers provide a nonempty buffer. */
static int nt_collect(FILE *fp, char *output, size_t size)
{
    size_t total = 0, n;
    char discard[1024];
    while (total < size - 1 &&
           (n = fread(output + total, 1, size - 1 - total, fp)) > 0)
        total += n;
    output[total] = '\0';
    while (fread(discard, 1, sizeof(discard), fp) > 0) {}
    int failed = ferror(fp);
    int status = pclose(fp);
    return failed ? -1 : status;
}

/* ---- ping 后台线程：解析丢包率 ---- */
static void *nt_run_ping(void *arg)
{
    int id = *(int *)arg;
    free(arg);

    char target[sizeof(nt_tests[0].target)];
    int idx = nt_begin(id, target, sizeof(target));
    if (idx < 0) return NULL;

    char cmd[NT_CMD_BUF];
    snprintf(cmd, sizeof(cmd), "ping -c 4 -W 3 '%.200s' 2>&1", target);

    FILE *fp = popen(cmd, "r");
    char output[1024];
    if (fp) {
        int ec = nt_collect(fp, output, sizeof(output));

        pthread_mutex_lock(&nt_mutex);
        if (idx >= 0 && nt_tests[idx].in_use && nt_tests[idx].test_id == id) {
            if (ec == 0) {
                /* 解析 packet loss: "X% packet loss" */
                int loss = -1;
                const char *lp = strstr(output, "% packet loss");
                if (lp) {
                    const char *s = lp;
                    while (s > output && s[-1] >= '0' && s[-1] <= '9') s--;
                    if (s < lp) loss = atoi(s);
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
                nt_finish(idx);
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
                nt_finish(idx);
            }
        }
        pthread_mutex_unlock(&nt_mutex);
    } else {
        pthread_mutex_lock(&nt_mutex);
        if (idx >= 0 && nt_tests[idx].in_use && nt_tests[idx].test_id == id) {
            snprintf(nt_tests[idx].error, NT_ERR_BUF, "\"无法执行 ping 命令\"");
            nt_finish(idx);
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

    char target[sizeof(nt_tests[0].target)];
    int idx = nt_begin(id, target, sizeof(target));
    if (idx < 0) return NULL;

    char cmd[NT_CMD_BUF];
    snprintf(cmd, sizeof(cmd),
        "curl -o /dev/null -s -w '%%{speed_download} %%{size_download} %%{time_total}' "
        "--noproxy '*' --max-time 10 --connect-timeout 5 '%.200s' 2>&1", target);

    FILE *fp = popen(cmd, "r");
    char output[256];
    if (fp) {
        int ec = nt_collect(fp, output, sizeof(output));

        pthread_mutex_lock(&nt_mutex);
        if (idx >= 0 && nt_tests[idx].in_use && nt_tests[idx].test_id == id) {
            if (ec == 0 && output[0]) {
                double speed = 0, size = 0, tm = 0;
                if (sscanf(output, "%lf %lf %lf", &speed, &size, &tm) == 3 && speed > 0) {
                    double mbps = (speed * 8.0) / 1000000.0;
                    snprintf(nt_tests[idx].result, NT_OUT_BUF,
                        "{\"speed_bps\":%.0f,\"speed_mbps\":%.2f,"
                        "\"size_bytes\":%.0f,\"time_sec\":%.2f}",
                        speed, mbps, size, tm);
                    nt_finish(idx);
                } else {
                    snprintf(nt_tests[idx].error, NT_ERR_BUF,
                        "\"无法解析测速结果\"");
                    nt_finish(idx);
                }
            } else {
                snprintf(nt_tests[idx].error, NT_ERR_BUF,
                    "\"下载失败，请检查网络连接\"");
                nt_finish(idx);
            }
        }
        pthread_mutex_unlock(&nt_mutex);
    } else {
        pthread_mutex_lock(&nt_mutex);
        if (idx >= 0 && nt_tests[idx].in_use && nt_tests[idx].test_id == id) {
            snprintf(nt_tests[idx].error, NT_ERR_BUF, "\"无法执行 curl 命令\"");
            nt_finish(idx);
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
    if (!id_arg) {
        pthread_mutex_lock(&nt_mutex);
        for (int i = 0; i < NT_MAX_TESTS; i++)
            if (nt_tests[i].in_use && nt_tests[i].test_id == test_id) nt_tests[i].in_use = 0;
        pthread_mutex_unlock(&nt_mutex);
        http_err(fd, 500, "Internal Server Error", "{\"error\":\"out of memory\"}");
        return;
    }
    *id_arg = test_id;
    int rc = http_worker_start(is_ping ? nt_run_ping : nt_run_speed, id_arg);
    if (rc != 0) {
        free(id_arg);
        pthread_mutex_lock(&nt_mutex);
        for (int i = 0; i < NT_MAX_TESTS; i++)
            if (nt_tests[i].in_use && nt_tests[i].test_id == test_id) nt_tests[i].in_use = 0;
        pthread_mutex_unlock(&nt_mutex);
        http_err(fd, 503, "Service Unavailable", "{\"error\":\"cannot start test\"}");
        return;
    }

    char json[128];
    int off = 0;
    JSON_ADD(json, off, "{\"test_id\":%d,\"status\":\"pending\"}", test_id);
    if (off < 0) http_err(fd, 500, "Error", "Network test response is too large");
    else http_ok_json(fd, json, (size_t)off);
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
    int idx = nt_find(test_id);

    char json[2560];
    int off = 0;
    if (idx < 0) {
        pthread_mutex_unlock(&nt_mutex);
        snprintf(json, sizeof(json), "{\"error\":\"test not found\"}");
        http_ok_json(fd, json, strlen(json));
        return;
    }

    /* 构建基础字段 */
    char escaped_target[sizeof(nt_tests[0].target) * 2];
    http_json_escape(nt_tests[idx].target, escaped_target, sizeof(escaped_target));
    off = http_json_append(json, sizeof(json), off,
        "{\"test_id\":%d,\"status\":\"%s\",\"type\":\"%s\",\"target\":\"%s\",\"result\":%s,\"error\":%s}",
        test_id, nt_tests[idx].status, nt_tests[idx].type, escaped_target,
        nt_tests[idx].result[0] ? nt_tests[idx].result : "null",
        nt_tests[idx].error[0] ? nt_tests[idx].error : "null");
    pthread_mutex_unlock(&nt_mutex);

    if (off < 0) http_err(fd, 500, "Error", "Network test response is too large");
    else http_ok_json(fd, json, (size_t)off);
}
