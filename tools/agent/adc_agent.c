/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * adc-agent.c — 板上智能体的命令行工具：把"看见"和"做到"做成两个可调用的动作。
 *
 * 存在意义：语言模型不参与控制回路。它只做两件事——
 *   see    读一次观测（检测框、距离分级、跟踪状态），可选把当前相机 JPEG 存盘；
 *   drive  把一段有限时长、有限包络的意图变成 8 Hz 的原语脉冲。
 * 其余（包络校验、里程核算、租约续期、任何退出路径上的停车）都在这里，
 * 因为这些事情不能交给一个可能超时、可能胡思乱想的模型。
 *
 * 用法：
 *   adc-agent see   --save /tmp/frame.jpg
 *   adc-agent drive --v 0.10 --steer 0.0 --ms 1500
 *   adc-agent plan  plan.json
 *   adc-agent stop | estop | release
 *   adc-agent token --scope motion --v-max 0.15 --budget 20   (管理员)
 *
 * 退出码：0 完成，2 计划或车况在发出第一条指令前被拒绝，3 运行中被中止，4 传输失败。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define HZ              8
#define TTL_MS          500
#define HTTP_TIMEOUT_MS 5000
#define PLAN_SEG_MAX    64
#define RESP_MAX        (512 * 1024)

typedef struct {
    double v, steer;
    int ms;
} seg_t;

typedef struct {
    char host[128];
    int port;
    char token[64];
    char user[64];
    char password[128];
    char save[512];
    int dry_run;
    int b64;
    long max_bytes;
    seg_t seg[PLAN_SEG_MAX];
    int seg_count;
    const char *plan_path;
    const char *scope;
    double ttl_s, v_max, steer_max, budget_m;
    char label[32];
    int persist, all;
    int have_motion;   /* --v/--ms 提供了单段计划 */
} opts_t;

static volatile sig_atomic_t g_abort;
static const char *g_base_host;
static int g_base_port;

static void on_signal(int sig) { (void)sig; g_abort = 1; }

static void jprint(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void jprint(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static unsigned long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

static void sleep_ms(unsigned long long ms)
{
    struct timespec ts = { .tv_sec = (time_t)(ms / 1000), .tv_nsec = (long)((ms % 1000) * 1000000L) };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
        if (g_abort) return;
    }
}

static void json_escape(const char *in, char *out, size_t size)
{
    size_t o = 0;
    out[0] = 0;
    for (const char *p = in; p && *p && o + 7 < size; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r' || c == '\t') { out[o++] = ' '; }
        else if (c < 0x20) { out[o++] = '?'; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

/* ---- 最小 HTTP 客户端 ----------------------------------------------------
 * 只支持本机明文 HTTP、无分块编码、Connection: close。这样实现只需要一次
 * connect + 一次 recv 循环，不必把 libcurl 拉进板子的部署面。 */
static int http_open(void)
{
    char port[16];
    snprintf(port, sizeof(port), "%d", g_base_port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_base_host, port, &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = HTTP_TIMEOUT_MS / 1000, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static int base64(const char *in, char *out, size_t out_size)
{
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(in), o = 0;
    if ((len + 2) / 3 * 4 + 1 > out_size) return -1;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < len) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)in[i + 2];
        out[o++] = tab[(v >> 18) & 63];
        out[o++] = tab[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tab[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tab[v & 63] : '=';
    }
    out[o] = 0;
    return (int)o;
}

/* 返回 HTTP 状态码；传输失败返回 -1；响应体写入 resp。 */
static int http_call(const opts_t *o, const char *method, const char *path,
                     const char *form, char *resp, size_t resp_size)
{
    int fd = http_open();
    if (fd < 0) return -1;

    char auth[256] = "";
    if (o->token[0]) snprintf(auth, sizeof(auth), "X-Agent-Cap: %s\r\n", o->token);
    else if (o->user[0]) {
        char raw[200], enc[280];
        snprintf(raw, sizeof(raw), "%s:%s", o->user, o->password);
        if (base64(raw, enc, sizeof(enc)) > 0)
            snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n", enc);
    }

    size_t flen = form ? strlen(form) : 0;
    char head[1024];
    int hn = snprintf(head, sizeof(head),
                      "%s %s HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "%s"
                      "Content-Length: %zu\r\n"
                      "%s"
                      "Connection: close\r\n\r\n",
                      method, path, g_base_host, g_base_port, auth, flen,
                      flen ? "Content-Type: application/x-www-form-urlencoded\r\n" : "");
    if (hn <= 0 || hn >= (int)sizeof(head)) { close(fd); return -1; }

    struct iovec {
        const char *buf;
        size_t len;
    } vec[2] = { { head, (size_t)hn }, { form, flen } };
    for (int i = 0; i < 2; i++) {
        size_t sent = 0;
        while (sent < vec[i].len) {
            ssize_t w = send(fd, vec[i].buf + sent, vec[i].len - sent, MSG_NOSIGNAL);
            if (w <= 0) { close(fd); return -1; }
            sent += (size_t)w;
        }
    }

    char *buf = malloc(resp_size);
    if (!buf) { close(fd); return -1; }
    size_t got = 0;
    for (;;) {
        if (got + 1 >= resp_size) break;
        ssize_t n = recv(fd, buf + got, resp_size - 1 - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    buf[got] = 0;

    int status = -1;
    if (!strncmp(buf, "HTTP/1.", 7)) status = atoi(buf + 9);
    const char *body = strstr(buf, "\r\n\r\n");
    body = body ? body + 4 : buf;
    snprintf(resp, resp_size, "%s", body);
    free(buf);
    return status;
}

/* ---- 观测 JSON 的取值 ---------------------------------------------------
 * 只解析本次用到的几个标量，且都限定在各自对象内，避免 "budget_m" 这类
 * 同名键跨对象误命中。 */
static const char *obj_of(const char *json, const char *key)
{
    if (!json) return NULL;
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    return p ? p + 1 : NULL;
}

static int num_in(const char *obj, const char *key, double *out)
{
    if (!obj) return -1;
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    char *end = NULL;
    double v = strtod(p + 1, &end);
    if (end == p + 1 || !isfinite(v)) return -1;
    *out = v;
    return 0;
}

static int bool_in(const char *obj, const char *key, int *out)
{
    if (!obj) return -1;
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(obj, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    if (!strncmp(p, "true", 4)) { *out = 1; return 0; }
    if (!strncmp(p, "false", 5)) { *out = 0; return 0; }
    return -1;
}

/* ---- 计划解析 -----------------------------------------------------------
 * 只认一种 schema：{"segments":[{"v":0.1,"steer":0,"ms":800}, ...]}。
 * 手写扫描器足以覆盖它，且不需要为板子引入一个 JSON 库。 */
static int find_key_range(const char *from, const char *key, const char **val)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(from, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    *val = p + 1;
    return 0;
}

static int plan_load(const char *path, seg_t *segs, int max, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open plan: %s", strerror(errno)); return -1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) {
        fclose(f);
        snprintf(err, errlen, "plan file is empty or larger than 256 KiB");
        return -1;
    }
    char *text = malloc((size_t)size + 1);
    if (!text) { fclose(f); snprintf(err, errlen, "out of memory"); return -1; }
    size_t got = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[got] = 0;

    const char *p;
    int n = 0;
    if (find_key_range(text, "segments", &p) < 0) {
        /* 允许省略外层对象，直接给数组 */
        p = strchr(text, '[');
        if (!p) { free(text); snprintf(err, errlen, "plan has no \"segments\" array"); return -1; }
    }
    p = strchr(p, '[');
    if (!p) { free(text); snprintf(err, errlen, "plan has no \"segments\" array"); return -1; }
    p++;
    while (*p && n < max) {
        const char *open = strchr(p, '{');
        if (!open) break;
        const char *close = strchr(open, '}');
        if (!close) break;
        char sub[512];
        size_t len = (size_t)(close - open) + 1;
        if (len >= sizeof(sub)) len = sizeof(sub) - 1;
        memcpy(sub, open, len);
        sub[len] = 0;
        double v = 0, steer = 0, ms = 0;
        if (num_in(sub, "v", &v) < 0 || num_in(sub, "ms", &ms) < 0) {
            free(text);
            snprintf(err, errlen, "segment %d: expected numeric v and ms", n);
            return -1;
        }
        (void)num_in(sub, "steer", &steer);
        if (ms <= 0 || ms > 10000) {
            free(text);
            snprintf(err, errlen, "segment %d: ms must be within 1..10000", n);
            return -1;
        }
        segs[n].v = v;
        segs[n].steer = steer;
        segs[n].ms = (int)ms;
        n++;
        p = close + 1;
    }
    free(text);
    if (!n) { snprintf(err, errlen, "plan has no segments"); return -1; }
    return n;
}

/* ---- 子命令 ---- */

/* 直接把帧流写到文件：响应体是二进制 JPEG，不能先塞进字符串再落盘。 */
static int frame_to_file(const opts_t *o, const char *path)
{
    int fd = http_open();
    if (fd < 0) return -1;
    char req[512];
    int hn = snprintf(req, sizeof(req),
                      "GET /api/agent/frame?max_bytes=%ld HTTP/1.1\r\n"
                      "Host: %s:%d\r\nX-Agent-Cap: %s\r\nConnection: close\r\n\r\n",
                      o->max_bytes > 0 ? o->max_bytes : 1048576L,
                      g_base_host, g_base_port, o->token);
    if (hn <= 0 || hn >= (int)sizeof(req)) { close(fd); return -1; }
    if (write(fd, req, (size_t)hn) != hn) { close(fd); return -1; }

    FILE *fp = fopen(path, "wb");
    if (!fp) { close(fd); return -1; }
    char chunk[65536];
    char hdr[4096];
    size_t hlen = 0, body_off = 0;
    int in_body = 0, wrote = 0, status = -1;
    ssize_t n;
    while ((n = recv(fd, chunk, sizeof(chunk), 0)) > 0) {
        size_t off = 0;
        if (!in_body) {
            size_t room = sizeof(hdr) - hlen - 1;
            size_t take = (size_t)n < room ? (size_t)n : room;
            memcpy(hdr + hlen, chunk, take);
            hlen += take;
            hdr[hlen] = 0;
            if (!strncmp(hdr, "HTTP/1.", 7)) status = atoi(hdr + 9);
            char *sep = strstr(hdr, "\r\n\r\n");
            if (sep) {
                in_body = 1;
                body_off = (size_t)(sep + 4 - hdr);
                /* sep 一定落在本次已读入的 chunk 内，回推本块起点 */
                off = body_off - (hlen - take);
            }
        }
        if (in_body && (size_t)n > off) {
            fwrite(chunk + off, 1, (size_t)n - off, fp);
            wrote += (int)((size_t)n - off);
        }
    }
    fclose(fp);
    close(fd);
    if (status != 200 || wrote <= 0) {
        unlink(path);
        return -1;
    }
    fprintf(stderr, "saved %s (%d bytes)\n", path, wrote);
    return 0;
}

static int cmd_observe(const opts_t *o)
{
    char *resp = malloc(RESP_MAX);
    if (!resp) return 4;
    int code = http_call(o, "GET", "/api/agent/observe", NULL, resp, RESP_MAX);
    if (code != 200) {
        char esc[256];
        json_escape(code < 0 ? "transport failure" : resp, esc, sizeof(esc));
        jprint("{\"error\":\"observe HTTP %d: %s\"}", code, esc);
        free(resp);
        return 4;
    }
    if (!strstr(resp, "\"limits\"")) {
        jprint("{\"error\":\"server did not report credential limits\"}");
        free(resp);
        return 4;
    }
    printf("%s\n", resp);
    fflush(stdout);

    if (o->save[0] && frame_to_file(o, o->save) < 0) fprintf(stderr, "no frame saved\n");
    free(resp);
    return 0;
}

static int cmd_frame(const opts_t *o)
{
    if (o->save[0] && !o->b64)
        return frame_to_file(o, o->save) == 0 ? 0 : 4;

    char *resp = malloc(RESP_MAX);
    if (!resp) return 4;
    char path[256];
    snprintf(path, sizeof(path), "/api/agent/frame?b64=%d&max_bytes=%ld", o->b64 ? 1 : 0,
             o->max_bytes > 0 ? o->max_bytes : 1048576L);
    int code = http_call(o, "GET", path, NULL, resp, RESP_MAX);
    if (code != 200) {
        char esc[256];
        json_escape(code < 0 ? "transport failure" : resp, esc, sizeof(esc));
        jprint("{\"error\":\"frame HTTP %d: %s\"}", code, esc);
        free(resp);
        return 4;
    }
    printf("%s\n", resp);
    free(resp);
    return 0;
}

static int cmd_simple(const opts_t *o, const char *method, const char *path, const char *form)
{
    char *resp = malloc(RESP_MAX);
    if (!resp) return 4;
    int code = http_call(o, method, path, form, resp, RESP_MAX);
    if (code < 0) { jprint("{\"error\":\"transport failure\"}"); free(resp); return 4; }
    char esc[512];
    json_escape(resp, esc, sizeof(esc));
    jprint("{\"http\":%d,\"body\":\"%s\"}", code, esc);
    free(resp);
    return code == 200 ? 0 : 3;
}

static const char *preflight(const char *observe_json, int dry_run, char *why, size_t whylen)
{
    const char *agent = obj_of(observe_json, "agent");
    const char *profile = obj_of(observe_json, "profile");
    const char *actuation = obj_of(observe_json, "actuation");
    int armed = 0, enabled = 0, ready = 0;
    if (bool_in(agent, "armed", &armed) < 0) armed = 0;
    if (bool_in(profile, "enabled", &enabled) < 0) enabled = 0;
    (void)bool_in(actuation, "ready", &ready);
    if (!armed) {
        snprintf(why, whylen, "credential is not armed; an operator must POST /api/agent/arm");
        return why;
    }
    if (!enabled) {
        snprintf(why, whylen, "follow profile is disabled; an operator must enable it first");
        return why;
    }
    if (!ready && !dry_run) {
        int motor = 0, can = 0;
        (void)bool_in(actuation, "motor_bound", &motor);
        (void)bool_in(actuation, "can_enabled", &can);
        snprintf(why, whylen, "actuation blocked: motor_bound=%s can_enabled=%s",
                 motor ? "true" : "false", can ? "true" : "false");
        return why;
    }
    return NULL;
}

static int check_plan(const opts_t *o, const double *v_max, const double *s_max,
                      const double *budget, double *total, char *why, size_t whylen)
{
    *total = 0;
    for (int i = 0; i < o->seg_count; i++) {
        const seg_t *s = &o->seg[i];
        if (s->v < 0 || s->v > *v_max) {
            snprintf(why, whylen, "segment %d: v=%.3f outside credential envelope 0..%.3f", i, s->v, *v_max);
            return -1;
        }
        if (fabs(s->steer) > *s_max) {
            snprintf(why, whylen, "segment %d: steer=%.3f outside credential envelope +/-%.3f", i, s->steer, *s_max);
            return -1;
        }
        *total += s->v * (double)s->ms / 1000.0;
    }
    if (*total > *budget) {
        snprintf(why, whylen, "plan needs %.2f m but the credential has %.2f m left", *total, *budget);
        return -1;
    }
    return 0;
}

static int cmd_drive(const opts_t *o)
{
    char *observe = malloc(RESP_MAX);
    if (!observe) return 4;
    int code = http_call(o, "GET", "/api/agent/observe", NULL, observe, RESP_MAX);
    if (code != 200) {
        char esc[256];
        json_escape(code < 0 ? "transport failure" : observe, esc, sizeof(esc));
        jprint("{\"error\":\"observe HTTP %d: %s\"}", code, esc);
        free(observe);
        return 4;
    }
    const char *limits = obj_of(observe, "limits");
    double v_max = 0, s_max = 0, budget = 0;
    if (!limits || num_in(limits, "v_max", &v_max) < 0 ||
        num_in(limits, "steer_max", &s_max) < 0 || num_in(limits, "budget_m", &budget) < 0) {
        jprint("{\"error\":\"server did not report credential limits\"}");
        free(observe);
        return 4;
    }
    char why[256];
    double planned = 0;
    if (check_plan(o, &v_max, &s_max, &budget, &planned, why, sizeof(why)) < 0) {
        char esc[320];
        json_escape(why, esc, sizeof(esc));
        jprint("{\"refused\":\"%s\"}", esc);
        free(observe);
        return 2;
    }
    const char *block = preflight(observe, o->dry_run, why, sizeof(why));
    if (block) {
        char esc[320];
        json_escape(block, esc, sizeof(esc));
        jprint("{\"refused\":\"%s\"}", esc);
        free(observe);
        return 2;
    }
    double total = 0;
    for (int i = 0; i < o->seg_count; i++) total += o->seg[i].v * (double)o->seg[i].ms / 1000.0;
    jprint("{\"plan_meters\":%.3f,\"segments\":%d,\"limits\":{\"v_max\":%.3f,\"steer_max\":%.3f,"
           "\"budget_m\":%.1f},\"dry_run\":%s}",
           total, o->seg_count, v_max, s_max, budget, o->dry_run ? "true" : "false");
    free(observe);
    if (o->dry_run) return 0;

    unsigned long long started = mono_ms();
    int sent = 0;
    for (int i = 0; i < o->seg_count; i++) {
        unsigned long long end = mono_ms() + (unsigned long long)o->seg[i].ms;
        while (mono_ms() < end && !g_abort) {
            char form[96];
            snprintf(form, sizeof(form), "v=%.4f&steer=%.4f&ttl_ms=%d",
                     o->seg[i].v, o->seg[i].steer, TTL_MS);
            char resp[4096];
            int rc = http_call(o, "POST", "/api/agent/motion", form, resp, sizeof(resp));
            if (rc != 200) {
                char esc[512];
                json_escape(rc < 0 ? "transport failure" : resp, esc, sizeof(esc));
                jprint("{\"abort\":true,\"segment\":%d,\"http\":%d,\"body\":\"%s\"}", i, rc, esc);
                cmd_simple(o, "POST", "/api/agent/stop", "");
                return 3;
            }
            sent++;
            sleep_ms(1000 / HZ);
        }
    }
    cmd_simple(o, "POST", "/api/agent/stop", "");
    jprint("{\"done\":true,\"commands\":%d,\"seconds\":%.2f,\"meters\":%.3f}",
           sent, (double)(mono_ms() - started) / 1000.0, total);
    return 0;
}

static int cmd_token(opts_t *o)
{
    char form[256];
    snprintf(form, sizeof(form),
             "scope=%s&ttl_s=%.0f&v_max=%.3f&steer_max=%.3f&budget_m=%.1f&label=%s%s",
             o->scope, o->ttl_s, o->v_max, o->steer_max, o->budget_m, o->label,
             o->persist ? "&persist=1" : "");
    return cmd_simple(o, "POST", "/api/agent/token", form);
}

static int cmd_revoke(opts_t *o)
{
    char form[96];
    if (o->all || !o->token[0]) snprintf(form, sizeof(form), "all=1");
    else snprintf(form, sizeof(form), "token=%s", o->token);
    return cmd_simple(o, "POST", "/api/agent/revoke", form);
}

static void usage(void)
{
    fprintf(stderr,
            "adc-agent — bounded agent tool: see, then act\n"
            "\n"
            "  adc-agent see    [--save FILE] [--max-bytes N]\n"
            "  adc-agent frame  [--save FILE] [--b64]\n"
            "  adc-agent drive  --v V --ms MS [--steer S] [--dry-run]\n"
            "  adc-agent plan   FILE [--dry-run]\n"
            "  adc-agent stop | release | estop | disarm\n"
            "  adc-agent token  --scope motion|observe [--ttl 900] [--v-max 0.15]\n"
            "                   [--steer-max 0.10] [--budget 20] [--label name] [--persist]\n"
            "  adc-agent arm    --token T\n"
            "  adc-agent revoke [--token T] [--all]\n"
            "  adc-agent status | audit\n"
            "\n"
            "Credentials: X-Agent-Cap from --token or $AGENT_CAP.\n"
            "Admin routes need --user/--password or $ADC_USER/$ADC_PASS.\n"
            "Exit codes: 0 done, 2 refused before motion, 3 aborted, 4 transport.\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    const char *cmd = argv[1];

    opts_t o;
    memset(&o, 0, sizeof(o));
    snprintf(o.host, sizeof(o.host), "http://127.0.0.1");
    o.port = 0;
    o.scope = "motion";
    o.ttl_s = 900;
    o.v_max = 0.15;
    o.steer_max = 0.10;
    o.budget_m = 20.0;
    snprintf(o.label, sizeof(o.label), "cli");
    o.max_bytes = 1048576;

    const char *env = getenv("AGENT_CAP");
    if (env) snprintf(o.token, sizeof(o.token), "%s", env);
    env = getenv("ADC_USER");
    if (env) snprintf(o.user, sizeof(o.user), "%s", env);
    env = getenv("ADC_PASS");
    if (env) snprintf(o.password, sizeof(o.password), "%s", env);

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "--host") && val) { snprintf(o.host, sizeof(o.host), "%s", val); i++; }
        else if (!strcmp(a, "--port") && val) { o.port = atoi(val); i++; }
        else if (!strcmp(a, "--token") && val) { snprintf(o.token, sizeof(o.token), "%s", val); i++; }
        else if (!strcmp(a, "--user") && val) { snprintf(o.user, sizeof(o.user), "%s", val); i++; }
        else if (!strcmp(a, "--password") && val) { snprintf(o.password, sizeof(o.password), "%s", val); i++; }
        else if (!strcmp(a, "--save") && val) { snprintf(o.save, sizeof(o.save), "%s", val); i++; }
        else if (!strcmp(a, "--max-bytes") && val) { o.max_bytes = atol(val); i++; }
        else if (!strcmp(a, "--b64")) o.b64 = 1;
        else if (!strcmp(a, "--dry-run")) o.dry_run = 1;
        else if (!strcmp(a, "--all")) o.all = 1;
        else if (!strcmp(a, "--persist")) o.persist = 1;
        else if (!strcmp(a, "--scope") && val) { o.scope = val; i++; }
        else if (!strcmp(a, "--ttl") && val) { o.ttl_s = atof(val); i++; }
        else if (!strcmp(a, "--v-max") && val) { o.v_max = atof(val); i++; }
        else if (!strcmp(a, "--steer-max") && val) { o.steer_max = atof(val); i++; }
        else if (!strcmp(a, "--budget") && val) { o.budget_m = atof(val); i++; }
        else if (!strcmp(a, "--label") && val) { snprintf(o.label, sizeof(o.label), "%s", val); i++; }
        else if (!strcmp(a, "--v") && val) { o.seg[0].v = atof(val); o.have_motion = 1; o.seg_count = 1; i++; }
        else if (!strcmp(a, "--steer") && val) { o.seg[0].steer = atof(val); i++; }
        else if (!strcmp(a, "--ms") && val) { o.seg[0].ms = atoi(val); i++; }
        else if (a[0] != '-') o.plan_path = a;
        else { fprintf(stderr, "unknown argument: %s\n", a); return 2; }
    }

    /* 解析 base URL：只接受 http://host[:port] */
    const char *hp = o.host;
    if (!strncmp(hp, "http://", 7)) hp += 7;
    else if (!strncmp(hp, "https://", 8)) {
        fprintf(stderr, "https is not supported by this build (loopback only)\n");
        return 2;
    }
    char hostbuf[128];
    snprintf(hostbuf, sizeof(hostbuf), "%s", hp);
    char *slash = strchr(hostbuf, '/');
    if (slash) *slash = 0;
    char *colon = strrchr(hostbuf, ':');
    int port = o.port > 0 ? o.port : 80;
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    g_base_host = hostbuf;
    g_base_port = port;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (!strcmp(cmd, "see")) {
        if (!o.token[0]) { jprint("{\"error\":\"missing credential: pass --token or set AGENT_CAP\"}"); return 4; }
        return cmd_observe(&o);
    }
    if (!strcmp(cmd, "frame")) {
        if (!o.token[0]) { jprint("{\"error\":\"missing credential: pass --token or set AGENT_CAP\"}"); return 4; }
        return cmd_frame(&o);
    }
    if (!strcmp(cmd, "drive") || !strcmp(cmd, "plan")) {
        if (!o.token[0]) { jprint("{\"error\":\"missing credential: pass --token or set AGENT_CAP\"}"); return 4; }
        if (!strcmp(cmd, "plan")) {
            char err[192];
            if (!o.plan_path) { jprint("{\"refused\":\"plan file required\"}"); return 2; }
            int n = plan_load(o.plan_path, o.seg, PLAN_SEG_MAX, err, sizeof(err));
            if (n < 0) {
                char esc[224];
                json_escape(err, esc, sizeof(esc));
                jprint("{\"refused\":\"%s\"}", esc);
                return 2;
            }
            o.seg_count = n;
        } else if (!o.have_motion || o.seg[0].ms <= 0) {
            jprint("{\"refused\":\"drive needs --v and --ms (or use plan FILE)\"}");
            return 2;
        }
        return cmd_drive(&o);
    }
    if (!strcmp(cmd, "stop") || !strcmp(cmd, "release")) {
        if (!o.token[0]) { jprint("{\"error\":\"missing credential\"}"); return 4; }
        return cmd_simple(&o, "POST", "/api/agent/stop", "");
    }
    if (!strcmp(cmd, "estop")) {
        if (!o.token[0]) { jprint("{\"error\":\"missing credential\"}"); return 4; }
        return cmd_simple(&o, "POST", "/api/agent/estop", "");
    }
    if (!strcmp(cmd, "disarm")) {
        if (!o.token[0]) { jprint("{\"error\":\"missing credential\"}"); return 4; }
        return cmd_simple(&o, "POST", "/api/agent/disarm", "");
    }
    if (!strcmp(cmd, "token")) {
        if (!o.user[0]) { jprint("{\"error\":\"admin credential required: --user/--password or ADC_USER/ADC_PASS\"}"); return 4; }
        return cmd_token(&o);
    }
    if (!strcmp(cmd, "arm")) {
        if (!o.user[0]) { jprint("{\"error\":\"admin credential required\"}"); return 4; }
        if (!o.token[0]) { jprint("{\"error\":\"token=<32 hex> is required\"}"); return 2; }
        char form[96];
        snprintf(form, sizeof(form), "token=%s", o.token);
        return cmd_simple(&o, "POST", "/api/agent/arm", form);
    }
    if (!strcmp(cmd, "revoke")) {
        if (!o.user[0]) { jprint("{\"error\":\"admin credential required\"}"); return 4; }
        return cmd_revoke(&o);
    }
    if (!strcmp(cmd, "status") || !strcmp(cmd, "audit")) {
        if (!o.user[0]) { jprint("{\"error\":\"admin credential required\"}"); return 4; }
        return cmd_simple(&o, "GET", !strcmp(cmd, "status") ? "/api/agent/state" : "/api/agent/audit", NULL);
    }
    usage();
    return 2;
}
