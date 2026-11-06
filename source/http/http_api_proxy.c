/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_proxy.c - Web editor for the board-wide network proxy.
 *
 * Independent of the Codex chat bridge: each request spawns
 * bin/proxy_config.py, which owns /etc/proxy-client.conf and (optionally)
 * reloads proxy-client.service. Every route requires root-level auth.
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

#define PROXY_SCRIPT "/opt/autonomous-driving-car/bin/proxy_config.py"
#define PROXY_TMPDIR "/opt/autonomous-driving-car/recordings/proxy-out"
#define PROXY_BUF    65536

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

/* Run a short-lived proxy command and return its JSON result in out. */
static int proxy_run(const char *cmd, const char *input, long input_len,
                     char *out, size_t out_size)
{
    static unsigned seq;
    char out_path[256], in_path[256];
    unsigned id = seq++;
    snprintf(out_path, sizeof(out_path), "%s/p%ld_%u.json",
             PROXY_TMPDIR, (long)getpid(), id);
    snprintf(in_path, sizeof(in_path), "%s/p%ld_%u.txt",
             PROXY_TMPDIR, (long)getpid(), id);

    int have_input = 0;
    if (input && input_len > 0) {
        FILE *fp = fopen(in_path, "wb");
        if (!fp) return -1;
        size_t n = (size_t)input_len;
        if (fwrite(input, 1, n, fp) != n) {
            fclose(fp);
            unlink(in_path);
            return -1;
        }
        fclose(fp);
        have_input = 1;
    }

    char *argv[10];
    int n = 0;
    argv[n++] = (char *)"python3";
    argv[n++] = (char *)PROXY_SCRIPT;
    argv[n++] = (char *)cmd;
    argv[n++] = (char *)"--out";
    argv[n++] = out_path;
    if (have_input) {
        argv[n++] = (char *)"--input";
        argv[n++] = in_path;
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
    if (!fp) {
        unlink(out_path);
        return -1;
    }
    size_t len = fread(out, 1, out_size - 1, fp);
    fclose(fp);
    unlink(out_path);
    out[len] = 0;
    return (int)len;
}

static void reply_proxy(int fd, const char *cmd, const char *input,
                        long input_len)
{
    char *buf = malloc(PROXY_BUF);
    if (!buf) {
        http_err(fd, 500, "Error", "out of memory");
        return;
    }
    int len = proxy_run(cmd, input, input_len, buf, PROXY_BUF);
    if (len <= 0) {
        static const char fallback[] =
            "{\"ok\":false,\"error\":\"proxy backend unavailable\"}";
        http_ok_json(fd, fallback, sizeof(fallback) - 1);
    } else {
        http_ok_json(fd, buf, (size_t)len);
    }
    free(buf);
}

void http_proxy_config(app_ctx_t *app, int fd, const char *method,
                       const char *uri, const char *req)
{
    (void)app; (void)uri;
    if (!strcmp(method, "POST")) {
        long len = 0;
        const char *body = body_of(req, &len);
        reply_proxy(fd, "set", body, len);
    } else {
        reply_proxy(fd, "get", NULL, 0);
    }
}

void http_proxy_test(app_ctx_t *app, int fd, const char *method,
                     const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    reply_proxy(fd, "test", NULL, 0);
}
