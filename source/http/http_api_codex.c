/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_codex.c — Web chat bridge to the local Codex CLI.
 *
 * The HTTP layer stays thin: every request spawns scripts/codex_bridge.py,
 * which owns the CLI process, the device-code login flow and the on-disk
 * conversation log. Long turns are detached by the bridge, so no HTTP worker
 * ever waits for a model. Every /api/codex/ route requires root-level auth.
 */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "http/http_internal.h"

#define CODEX_ROOT   "/opt/autonomous-driving-car"
#define CODEX_BRIDGE CODEX_ROOT "/bin/codex_bridge.py"
#define CODEX_OUT    CODEX_ROOT "/.codex/out"
#define CODEX_INBOX  CODEX_ROOT "/.codex/inbox"
#define CODEX_BUF    262144

extern char **environ;

static const char *body_of(const char *req, long *len)
{
    const char *body = http_body_start(req);
    long cl = http_content_length(req);
    if (cl < 0) cl = (long)strlen(body);
    if (cl < 0 || cl > (long)HTTP_BUF_SIZE) cl = 0;
    *len = cl;
    return body;
}

/* Query-string lookup that tolerates an absent value and %XX escapes. */
static int query_param(const char *uri, const char *key, char *out, size_t size)
{
    const char *q = strchr(uri ? uri : "", '?');
    if (!q) return 0;
    q++;
    size_t klen = strlen(key);
    while (*q) {
        const char *amp = strchr(q, '&');
        size_t seg = amp ? (size_t)(amp - q) : strlen(q);
        if (seg > klen && !strncmp(q, key, klen) && q[klen] == '=') {
            char raw[512];
            size_t n = seg - klen - 1;
            if (n >= sizeof(raw)) n = sizeof(raw) - 1;
            memcpy(raw, q + klen + 1, n);
            raw[n] = 0;
            return http_url_decode(raw, out, size);
        }
        q = amp ? amp + 1 : q + seg;
    }
    return 0;
}

/* Run a short-lived bridge command and return its JSON result in out. */
static int bridge_run(const char *cmd, const char *input, long input_len,
                      const char *arg, char *out, size_t out_size)
{
    static unsigned seq;
    char out_path[256], in_path[256];
    unsigned id = seq++;
    snprintf(out_path, sizeof(out_path), "%s/r%ld_%u.json",
             CODEX_OUT, (long)getpid(), id);
    snprintf(in_path, sizeof(in_path), "%s/i%ld_%u.txt",
             CODEX_INBOX, (long)getpid(), id);

    int have_input = 0;
    if (input && input_len > 0) {
        FILE *fp = fopen(in_path, "wb");
        if (!fp) return -1;
        size_t n = (size_t)input_len;
        if (fwrite(input, 1, n, fp) != n) { fclose(fp); unlink(in_path); return -1; }
        fclose(fp);
        have_input = 1;
    }

    char *argv[12];
    int n = 0;
    argv[n++] = (char *)"python3";
    argv[n++] = (char *)CODEX_BRIDGE;
    argv[n++] = (char *)cmd;
    argv[n++] = (char *)"--out";
    argv[n++] = out_path;
    if (have_input) {
        argv[n++] = (char *)"--input";
        argv[n++] = in_path;
    }
    if (arg && arg[0]) {
        argv[n++] = (char *)"--arg";
        argv[n++] = (char *)arg;
    }
    argv[n] = NULL;

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        if (have_input) unlink(in_path);
        return -1;
    }
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "python3", &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc == 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    } else {
        errno = rc;
    }
    if (have_input) unlink(in_path);

    FILE *fp = fopen(out_path, "rb");
    if (!fp) { unlink(out_path); return -1; }
    size_t len = fread(out, 1, out_size - 1, fp);
    fclose(fp);
    unlink(out_path);
    out[len] = 0;
    return (int)len;
}

static void reply_bridge(int fd, const char *cmd, const char *input,
                         long input_len, const char *arg)
{
    char *buf = malloc(CODEX_BUF);
    if (!buf) { http_err(fd, 500, "Error", "out of memory"); return; }
    int len = bridge_run(cmd, input, input_len, arg, buf, CODEX_BUF);
    if (len <= 0) {
        static const char fallback[] =
            "{\"ok\":false,\"error\":\"Codex bridge unavailable\"}";
        http_ok_json(fd, fallback, sizeof(fallback) - 1);
    } else {
        http_ok_json(fd, buf, (size_t)len);
    }
    free(buf);
}

static int json_str(const char *buf, const char *key, char *out, size_t size)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(buf, pattern);
    if (!p) return -1;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (*p++ != '"') return -1;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t n = (size_t)(end - p);
    if (n >= size) n = size - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 0;
}

/* The bridge is Python: json.dump() emits "\"ok\": true" with a space. */
static int json_ok(const char *buf)
{
    const char *p = strstr(buf, "\"ok\":");
    if (!p) return 0;
    p += 5;
    while (*p == ' ' || *p == '\t') p++;
    return !strncmp(p, "true", 4);
}

static void serve_path(int fd, const char *path, const char *name,
                       const char *unlink_after)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { http_err(fd, 404, "Not Found", "record not found"); return; }
    size_t size = http_file_size(fp);
    char disp[320];
    snprintf(disp, sizeof(disp),
             "Content-Disposition: attachment; filename=\"%s\"\r\n"
             "Cache-Control: no-store\r\n", name);
    http_serve_stream(fd, "application/octet-stream", disp, fp, size, unlink_after);
}

void http_codex_status(app_ctx_t *app, int fd, const char *method,
                       const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    reply_bridge(fd, "status", NULL, 0, NULL);
}

/* Read-only CLI version check; may take a few seconds over the board proxy. */
void http_codex_update_check(app_ctx_t *app, int fd, const char *method,
                             const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    reply_bridge(fd, "check-update", NULL, 0, NULL);
}

void http_codex_config_post(app_ctx_t *app, int fd, const char *method,
                            const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri;
    long len = 0;
    const char *body = body_of(req, &len);
    reply_bridge(fd, "config", body, len, NULL);
}

void http_codex_login_device(app_ctx_t *app, int fd, const char *method,
                             const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    reply_bridge(fd, "login-device", NULL, 0, NULL);
}

void http_codex_chat_post(app_ctx_t *app, int fd, const char *method,
                          const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri;
    long len = 0;
    const char *body = body_of(req, &len);
    reply_bridge(fd, "chat-start", body, len, NULL);
}

void http_codex_reset(app_ctx_t *app, int fd, const char *method,
                      const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    reply_bridge(fd, "reset", NULL, 0, NULL);
}

void http_codex_stop(app_ctx_t *app, int fd, const char *method,
                     const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    reply_bridge(fd, "stop", NULL, 0, NULL);
}

void http_codex_logout(app_ctx_t *app, int fd, const char *method,
                       const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    reply_bridge(fd, "logout", NULL, 0, NULL);
}

void http_codex_history(app_ctx_t *app, int fd, const char *method,
                        const char *uri, const char *req)
{
    (void)app; (void)method; (void)req;
    char after[32] = "0";
    query_param(uri, "after", after, sizeof(after));
    reply_bridge(fd, "history", NULL, 0, after);
}

void http_codex_records(app_ctx_t *app, int fd, const char *method,
                        const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    reply_bridge(fd, "records", NULL, 0, NULL);
}

void http_codex_records_clear(app_ctx_t *app, int fd, const char *method,
                              const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    reply_bridge(fd, "records-clear", NULL, 0, NULL);
}

void http_codex_record(app_ctx_t *app, int fd, const char *method,
                       const char *uri, const char *req)
{
    (void)app; (void)method; (void)req;
    char name[128] = "";
    if (!query_param(uri, "name", name, sizeof(name)) || !name[0]) {
        http_err(fd, 400, "Bad Request", "missing record name");
        return;
    }
    char *buf = malloc(CODEX_BUF);
    if (!buf) { http_err(fd, 500, "Error", "out of memory"); return; }
    if (bridge_run("record", NULL, 0, name, buf, CODEX_BUF) > 0 &&
        json_ok(buf)) {
        char path[256], shown[128];
        if (json_str(buf, "path", path, sizeof(path)) == 0 &&
            json_str(buf, "name", shown, sizeof(shown)) == 0) {
            serve_path(fd, path, shown, NULL);
        } else {
            http_err(fd, 500, "Error", "bad bridge reply");
        }
    } else {
        http_err(fd, 404, "Not Found", "record not found");
    }
    free(buf);
}

void http_codex_export(app_ctx_t *app, int fd, const char *method,
                       const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    char *buf = malloc(CODEX_BUF);
    if (!buf) { http_err(fd, 500, "Error", "out of memory"); return; }
    if (bridge_run("export", NULL, 0, NULL, buf, CODEX_BUF) > 0 &&
        json_ok(buf)) {
        char path[256], name[128];
        if (json_str(buf, "path", path, sizeof(path)) == 0 &&
            json_str(buf, "name", name, sizeof(name)) == 0) {
            serve_path(fd, path, name, path);
        } else {
            http_err(fd, 500, "Error", "bad bridge reply");
        }
    } else {
        http_err(fd, 500, "Error", "cannot export records");
    }
    free(buf);
}
