/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_proxy.c — 整机网络代理的 Web 编辑器（原生实现）。
 *
 * 这里管理的是 /etc/proxy-client.conf，即 proxy-client.service 的
 * EnvironmentFile：该 unit 会据此重写 /etc/environment、/etc/profile.d/proxy.sh
 * 与 systemd 的全局代理，所以保存一次影响的是整块板子，而不是本进程。
 *
 * 历史实现把这件事交给一个外部 Python 脚本；现在整条链路都在进程内：
 * 解析 → 校验 → 备份 → 就地写入 → 重启/停止 unit。进程内做的直接好处是
 * 不再有 python3 解释器、临时文件与被拉起的子进程出现在 HTTP 路径上。
 *
 * 写入方式必须是"就地覆盖"而不是"临时文件 + rename"：systemd 的
 * ProtectSystem=strict 只把这一个文件挂成可写，所在目录 /etc 是只读的。
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "http/http_internal.h"

#define PROXY_CONF      "/etc/proxy-client.conf"
#define PROXY_SERVICE   "proxy-client.service"
#define PROXY_BACKUP    PATH_RECORDINGS "/proxy-backups"
#define PROXY_RAW_MAX   8192

static const char *kKeys[] = { "PROXY_HOST", "PROXY_PORT", "PROXY_SCHEME", "NO_PROXY" };
static const char *kDefaults[] = { "127.0.0.1", "7890", "http", "localhost,127.0.0.1,::1" };
#define KEY_N ((int)(sizeof(kKeys) / sizeof(kKeys[0])))

typedef struct {
    char value[KEY_N][128];
    char raw[PROXY_RAW_MAX + 1];
    int exists;
} proxy_conf_t;

/* ---- 小工具 ---- */
/* snprintf 返回的是"本来想写的长度"，截断时它会大于剩余空间。
 * 把它直接加到偏移上会让 off 越过缓冲，下一轮 sizeof - off 下溢成巨大的 size_t，
 * 指针跑到栈外。所有拼接都必须走这里：只在完整写入时前进偏移。 */
static int sadd(char *buf, size_t size, int off, const char *fmt, ...)
{
    if (off < 0 || (size_t)off >= size) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, size - (size_t)off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size - (size_t)off) return -1;
    return off + n;
}

/* 配置文件里换行是有意义的结构，不能像普通 JSON 字符串那样把它吃掉。 */
static void escape_text(const char *in, char *out, size_t size)
{
    size_t o = 0;
    out[0] = 0;
    for (const char *p = in; p && *p && o + 8 < size; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c == '\r') continue;
        else if (c < 0x20) { out[o++] = ' '; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static int read_all(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { out[0] = 0; return 0; }
    size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = 0;
    return (int)n;
}

/* 运行一条短命令并收集 stdout；返回退出码（无法执行返回 -1）。 */
static int run_cmd(const char *cmd, char *out, size_t size)
{
    FILE *p = popen(cmd, "r");
    if (!p) { if (out && size) out[0] = 0; return -1; }
    size_t n = 0;
    if (out && size > 1) n = fread(out, 1, size - 1, p);
    if (out && size) {
        out[n] = 0;
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    }
    int status = pclose(p);
    if (status < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void trim(char *s)
{
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
}

/* 解析作业：KEY="value" / KEY='value' / KEY=value # comment */
static int conf_parse_line(const char *line, char *key, size_t ksize, char *val, size_t vsize)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == ';' || !*p) return -1;
    if (!strncmp(p, "export ", 7)) { p += 7; while (*p == ' ') p++; }
    size_t k = 0;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_' || (*p >= '0' && *p <= '9')) {
        if (k + 1 >= ksize) return -1;
        key[k++] = *p++;
    }
    if (!k) return -1;
    key[k] = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    size_t v = 0;
    if (*p == '"' || *p == '\'') {
        char quote = *p++;
        while (*p && *p != quote && v + 1 < vsize) val[v++] = *p++;
    } else {
        while (*p && *p != '#' && v + 1 < vsize) val[v++] = *p++;
    }
    val[v] = 0;
    trim(val);
    return 0;
}

static void conf_read(proxy_conf_t *conf)
{
    memset(conf, 0, sizeof(*conf));
    for (int i = 0; i < KEY_N; i++) snprintf(conf->value[i], sizeof(conf->value[i]), "%s", kDefaults[i]);
    conf->exists = read_all(PROXY_CONF, conf->raw, sizeof(conf->raw)) > 0;

    char *copy = strdup(conf->raw);
    if (!copy) return;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char key[64], val[128];
        if (conf_parse_line(line, key, sizeof(key), val, sizeof(val)) < 0) continue;
        for (int i = 0; i < KEY_N; i++)
            if (!strcmp(key, kKeys[i]) && val[0]) snprintf(conf->value[i], sizeof(conf->value[i]), "%s", val);
    }
    free(copy);
}

static void proxy_url(const proxy_conf_t *conf, char *out, size_t size)
{
    snprintf(out, size, "%s://%s:%s", conf->value[2], conf->value[0], conf->value[1]);
}

static int all_digits(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return 0;
    return 1;
}

static int host_ok(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '.' || *p == '-')) return 0;
    return 1;
}

static int bypass_ok(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              strchr(".,:/_-", *p))) return 0;
    return 1;
}

static int conf_valid(const proxy_conf_t *c)
{
    long port = strtol(c->value[1], NULL, 10);
    int scheme_ok = !strcmp(c->value[2], "http") || !strcmp(c->value[2], "socks5") ||
                    !strcmp(c->value[2], "socks5h");
    return host_ok(c->value[0]) && all_digits(c->value[1]) && port > 0 && port < 65536 &&
           scheme_ok && bypass_ok(c->value[3]);
}

/* 保留注释与无关行，只替换受管赋值；缺失的键追加到末尾。 */
static int conf_render(const proxy_conf_t *c, const char *original, char *out, size_t size)
{
    int off = 0, seen[KEY_N] = { 0 };
    const char *p = original;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[512];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;

        char key[64], val[128];
        int managed = -1;
        if (conf_parse_line(line, key, sizeof(key), val, sizeof(val)) == 0)
            for (int i = 0; i < KEY_N; i++) if (!strcmp(key, kKeys[i])) managed = i;
        if (managed >= 0) {
            int n = snprintf(out + off, size - (size_t)off, "%s=\"%s\"\n", kKeys[managed], c->value[managed]);
            if (n < 0 || (size_t)n >= size - (size_t)off) return -1;
            off += n;
            seen[managed] = 1;
        } else if (len) {
            int n = snprintf(out + off, size - (size_t)off, "%s\n", line);
            if (n < 0 || (size_t)n >= size - (size_t)off) return -1;
            off += n;
        }
        p = eol ? eol + 1 : NULL;
    }
    for (int i = 0; i < KEY_N; i++) {
        if (seen[i]) continue;
        int n = snprintf(out + off, size - (size_t)off, "%s=\"%s\"\n", kKeys[i], c->value[i]);
        if (n < 0 || (size_t)n >= size - (size_t)off) return -1;
        off += n;
    }
    return off;
}

static int conf_write(const proxy_conf_t *c, char *backup, size_t bsize)
{
    backup[0] = 0;
    char rendered[4096];
    int len = conf_render(c, c->raw, rendered, sizeof(rendered));
    if (len < 0) return -1;

    if (c->exists) {
        (void)mkdir(PATH_RECORDINGS, 0755);
        (void)mkdir(PROXY_BACKUP, 0755);
        time_t now = time(NULL);
        struct tm parts;
        if (localtime_r(&now, &parts))
            strftime(backup, bsize, PROXY_BACKUP "/proxy-client.conf.%Y%m%d_%H%M%S", &parts);
        if (backup[0]) {
            FILE *b = fopen(backup, "wb");
            if (b) {
                fwrite(c->raw, 1, strlen(c->raw), b);
                fclose(b);
                (void)chmod(backup, 0644);
            } else {
                backup[0] = 0;
            }
        }
    }

    int fd = open(PROXY_CONF, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    ssize_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, rendered + written, (size_t)(len - written));
        if (n <= 0) { close(fd); return -1; }
        written += n;
    }
    (void)fsync(fd);
    close(fd);
    return 0;
}

static void service_state(char *state, size_t size)
{
    if (run_cmd("systemctl is-active " PROXY_SERVICE " 2>/dev/null", state, size) != 0 || !state[0])
        snprintf(state, size, "unknown");
}

static int service_set(int enabled, char *err, size_t errlen)
{
    char cmd[180], out[512];
    /* --no-block：restart 的等待是秒级的，而 HTTP 只有一条事件循环线程，
     * 它卡住的每一毫秒都在推迟 /api/agent/motion 的续租。 */
    snprintf(cmd, sizeof(cmd), "systemctl %s %s " PROXY_SERVICE " 2>&1",
             enabled ? "--no-block restart" : "--no-block stop");
    int rc = run_cmd(cmd, out, sizeof(out));
    if (rc != 0) {
        snprintf(err, errlen, "%.300s", out[0] ? out : "systemctl failed");
        return -1;
    }
    return 0;
}

/* ---- 极简 POST 体解析（仅本接口使用的固定 schema） ---- */
static int body_string(const char *body, const char *key, char *out, size_t size)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < size) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = 0;
    return 0;
}

/* present=0 表示请求体没提这个键（保持现状，而不是默认 false）。 */
static int body_bool(const char *body, const char *key, int *present)
{
    *present = 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    *present = 1;
    return !strncmp(p, "true", 4) || !strncmp(p, "1", 1);
}

/* ---- 响应构造 ---- */
static void reply_config(int fd, const proxy_conf_t *c, const char *backup, int applied,
                         const char *apply_error)
{
    char url[192], service[32], sysenv[2048];
    proxy_url(c, url, sizeof(url));
    service_state(service, sizeof(service));

    /* 板级环境变量：proxy-client.service 写进 /etc/environment 的最终值 */
    char envraw[4096];
    read_all("/etc/environment", envraw, sizeof(envraw));
    const char *wanted[] = { "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY" };
    int eoff = sadd(sysenv, sizeof(sysenv), 0, "{");
    for (int i = 0; i < 4; i++) {
        char found[256] = "";
        char *copy = strdup(envraw);
        if (copy) {
            char *save = NULL;
            for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
                char key[64], val[256];
                if (conf_parse_line(line, key, sizeof(key), val, sizeof(val)) == 0 && !strcmp(key, wanted[i]))
                    snprintf(found, sizeof(found), "%s", val);
            }
            free(copy);
        }
        char esc[300];
        http_json_escape(found, esc, sizeof(esc));
        eoff = sadd(sysenv, sizeof(sysenv), eoff, "%s\"%s\":\"%s\"",
                    i ? "," : "", wanted[i], esc);
    }
    eoff = sadd(sysenv, sizeof(sysenv), eoff, "}");
    if (eoff < 0) snprintf(sysenv, sizeof(sysenv), "{}");

    char esc_raw[PROXY_RAW_MAX * 6 / 5 + 32];
    escape_text(c->raw, esc_raw, sizeof(esc_raw));
    char esc_backup[320], esc_url[256], esc_err[360];
    http_json_escape(backup ? backup : "", esc_backup, sizeof(esc_backup));
    http_json_escape(url, esc_url, sizeof(esc_url));
    http_json_escape(apply_error ? apply_error : "", esc_err, sizeof(esc_err));

    size_t cap = strlen(esc_raw) + 4096;
    char *json = malloc(cap);
    if (!json) { http_err(fd, 500, "Error", "out of memory"); return; }
    int off = snprintf(json, cap,
                       "{\"ok\":true,\"path\":\"" PROXY_CONF "\",\"exists\":%s,"
                       "\"config\":{\"PROXY_HOST\":\"%s\",\"PROXY_PORT\":\"%s\","
                       "\"PROXY_SCHEME\":\"%s\",\"NO_PROXY\":\"%s\"},"
                       "\"url\":\"%s\",\"raw\":\"%s\",\"system\":%s,"
                       "\"service\":\"%s\",\"enabled\":%s,\"backup\":\"%s\","
                       "\"applied\":%s,\"apply_error\":\"%s\"}",
                       c->exists ? "true" : "false",
                       c->value[0], c->value[1], c->value[2], c->value[3],
                       esc_url, esc_raw, sysenv, service,
                       !strcmp(service, "active") ? "true" : "false",
                       esc_backup, applied ? "true" : "false", esc_err);
    /* off >= cap 表示被截断：此时 http_ok_json 会按 off 读出去，必须当错误处理。 */
    if (off < 0 || (size_t)off >= cap) { free(json); http_err(fd, 500, "Error", "encode failed"); return; }
    http_ok_json(fd, json, (size_t)off);
    free(json);
}

/* ---- 路由 ---- */
void http_proxy_config(app_ctx_t *app, int fd, const char *method,
                       const char *uri, const char *req)
{
    (void)app; (void)uri;
    if (strcmp(method, "POST") != 0) {
        proxy_conf_t conf;
        conf_read(&conf);
        reply_config(fd, &conf, "", 0, "");
        return;
    }

    const char *body = http_body_start(req);
    if (!body || !*body) {
        http_err(fd, 400, "Bad Request", "expected a JSON body with PROXY_HOST/PROXY_PORT");
        return;
    }

    proxy_conf_t conf;
    conf_read(&conf);
    for (int i = 0; i < KEY_N; i++) {
        char val[128];
        if (body_string(body, kKeys[i], val, sizeof(val)) == 0) {
            size_t len = strlen(val);
            if (len > 255) len = 255;
            memcpy(conf.value[i], val, len);
            conf.value[i][len] = 0;
        }
    }
    if (!conf_valid(&conf)) {
        static const char msg[] =
            "{\"ok\":false,\"error\":\"invalid proxy host, port, scheme or bypass list\"}";
        http_ok_json(fd, msg, sizeof(msg) - 1);
        return;
    }

    char backup[320] = "";
    if (conf_write(&conf, backup, sizeof(backup)) < 0) {
        char json[512];
        int off = snprintf(json, sizeof(json),
                           "{\"ok\":false,\"error\":\"cannot write " PROXY_CONF ": %s\"}", strerror(errno));
        http_ok_json(fd, json, (size_t)off);
        return;
    }

    /* 缺省 enabled 表示"沿用当前开关"：只改字段不应该偷偷把代理打开或关掉。 */
    char before[32], after[32];
    service_state(before, sizeof(before));
    int enabled_present = 0, enabled = body_bool(body, "enabled", &enabled_present);
    if (!enabled_present) enabled = !strcmp(before, "active");
    int apply_present = 0;
    int apply = body_bool(body, "apply", &apply_present);

    int applied = 0;
    char apply_error[320] = "";
    if (apply_present || enabled != (!strcmp(before, "active"))) {
        applied = service_set(enabled, apply_error, sizeof(apply_error)) == 0;
    }
    service_state(after, sizeof(after));
    reply_config(fd, &conf, backup, applied, apply_error);
}

/* ---- POST /api/proxy/test ----
 * TCP 可达性 + 通过该代理取一次 HTTPS 状态码。401 是正常结果：接口在，但没带凭据。 */
static int tcp_probe(const char *host, int port, char *err, size_t errlen)
{
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_s, &hints, &res) != 0) {
        snprintf(err, errlen, "cannot resolve %s", host);
        return 0;
    }
    int ok = 0;
    for (struct addrinfo *ai = res; ai && !ok; ai = ai->ai_next) {
        int s = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (s < 0) continue;
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(s, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            ok = 1;
        } else if (errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = s, .events = POLLOUT };
            if (poll(&pfd, 1, 1500) == 1) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 && soerr == 0) ok = 1;
                else snprintf(err, errlen, "%s", strerror(soerr));
            } else {
                snprintf(err, errlen, "connect timeout");
            }
        } else {
            snprintf(err, errlen, "%s", strerror(errno));
        }
        close(s);
    }
    freeaddrinfo(res);
    return ok;
}

/* 真正的探测：最坏情况 curl 5 s + tcp 1.5 s。它绝不能跑在事件循环里——那个线程
 * 同时负责 /api/agent/motion 的应答，卡过 500 ms 就会让租约到期、车被硬停。 */
static void proxy_test_run(char *json, size_t size)
{
    proxy_conf_t conf;
    conf_read(&conf);
    if (!conf_valid(&conf)) {
        snprintf(json, size, "{\"ok\":false,\"error\":\"invalid proxy configuration\"}");
        return;
    }
    char url[192], tcp_error[200] = "";
    proxy_url(&conf, url, sizeof(url));
    int tcp_ok = tcp_probe(conf.value[0], atoi(conf.value[1]), tcp_error, sizeof(tcp_error));

    char cmd[640], out[128];
    snprintf(cmd, sizeof(cmd),
             "env -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY -u http_proxy -u https_proxy -u all_proxy "
             "curl -sS -o /dev/null -w '%%{http_code}' --proxy '%s' "
             "--connect-timeout 3 --max-time 5 https://api.openai.com/v1/models 2>/dev/null", url);
    int rc = run_cmd(cmd, out, sizeof(out));
    int http_code = 0;
    for (const char *p = out; *p; p++) if (*p >= '0' && *p <= '9') http_code = http_code * 10 + (*p - '0');
    char curl_error[220] = "";
    if (rc != 0) snprintf(curl_error, sizeof(curl_error), "curl exit %d", rc);

    int reachable = tcp_ok && (http_code == 200 || http_code == 301 || http_code == 302 ||
                               http_code == 400 || http_code == 401 || http_code == 403 ||
                               http_code == 404);
    char esc_url[256], esc_tcp[300], esc_curl[300];
    http_json_escape(url, esc_url, sizeof(esc_url));
    http_json_escape(tcp_error, esc_tcp, sizeof(esc_tcp));
    http_json_escape(curl_error, esc_curl, sizeof(esc_curl));
    snprintf(json, size,
             "{\"ok\":true,\"url\":\"%s\",\"tcp\":%s,\"tcp_error\":\"%s\","
             "\"http_code\":%d,\"curl_error\":\"%s\",\"reachable\":%s}",
             esc_url, tcp_ok ? "true" : "false", esc_tcp, http_code, esc_curl,
             reachable ? "true" : "false");
}

/* 单槽结果：同一时刻只允许一次探测在跑，第二次 POST 返回 429 而不是排队。
 * 这也是报告里建议的"同一客户端冷却"，只是把窗口收成"上一次还没结束"。 */
#define PT_JSON_MAX 1024
typedef struct {
    int    pending;
    char   json[PT_JSON_MAX];
    time_t finished_at;
} proxy_test_t;

static proxy_test_t    g_pt;
static pthread_mutex_t g_pt_lock = PTHREAD_MUTEX_INITIALIZER;

static void *proxy_test_worker(void *arg)
{
    (void)arg;
    char buf[PT_JSON_MAX];
    proxy_test_run(buf, sizeof(buf));
    pthread_mutex_lock(&g_pt_lock);
    safe_strncpy(g_pt.json, sizeof(g_pt.json), buf);
    g_pt.pending = 0;
    g_pt.finished_at = time(NULL);
    pthread_mutex_unlock(&g_pt_lock);
    return NULL;
}

/* ---- /api/proxy/test ----
 * POST：提交一次探测，立即返回 {"ok":true,"pending":true}；
 * GET ：轮询上一次的结果，仍在跑时同样返回 pending。
 * 契约从"同步等结果"变成"提交 + 轮询"，因为同步等待曾让整个 Web 面被串行化。 */
void http_proxy_test(app_ctx_t *app, int fd, const char *method,
                     const char *uri, const char *req)
{
    (void)app; (void)uri; (void)req;

    static const char kPending[] = "{\"ok\":true,\"pending\":true}";

    if (strcmp(method, "POST") != 0) {
        char body[PT_JSON_MAX];
        int pending;
        time_t done;
        pthread_mutex_lock(&g_pt_lock);
        pending = g_pt.pending;
        done = g_pt.finished_at;
        safe_strncpy(body, sizeof(body), g_pt.json);
        pthread_mutex_unlock(&g_pt_lock);

        if (pending) { http_ok_json(fd, kPending, sizeof(kPending) - 1); return; }
        if (body[0]) { http_ok_json(fd, body, strlen(body)); return; }
        char empty[128];
        int n = snprintf(empty, sizeof(empty),
                         "{\"ok\":true,\"pending\":false,\"never_run\":true}");
        (void)done;
        http_ok_json(fd, empty, (size_t)n);
        return;
    }

    pthread_mutex_lock(&g_pt_lock);
    if (g_pt.pending) {
        pthread_mutex_unlock(&g_pt_lock);
        http_err(fd, 429, "Too Many Requests",
                 "{\"ok\":false,\"error\":\"a proxy test is already running\"}");
        return;
    }
    g_pt.pending = 1;
    g_pt.json[0] = 0;
    pthread_mutex_unlock(&g_pt_lock);

    if (http_worker_start(proxy_test_worker, NULL) != 0) {
        pthread_mutex_lock(&g_pt_lock);
        g_pt.pending = 0;
        pthread_mutex_unlock(&g_pt_lock);
        http_err(fd, 503, "Service Unavailable",
                 "{\"ok\":false,\"error\":\"cannot start proxy test\"}");
        return;
    }
    http_ok_json(fd, kPending, sizeof(kPending) - 1);
}
