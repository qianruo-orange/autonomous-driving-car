/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <ctype.h>
#include <stdint.h>
/**
 * http_logs.c — 日志管理路由（列表 / 下载 / 删除 / 打包）。
 */

#include "http/http_internal.h"
#include "watchdog/watchdog.h"

/* 日志目录不再硬编码 PATH_LOGS：与核心日志保持一致，使用配置的 log_dir */

/* 一级平铺的文件名：不允许路径分隔符，避免穿越 recordings/logs/ */
static int log_name_ok(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    if (n < 5 || n >= 256 || name[0] == '.') return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

/* 日志文件列表 JSON API（日志目录一级平铺，不再按日期分层） */
static void serve_logs(int fd, const char *log_dir, int clear)
{
    DIR *dir = opendir(log_dir);
    if (!dir) {
        http_err(fd, 500, "Error", NULL);
        return;
    }

    char json[32768];
    int off = snprintf(json, sizeof(json), "{\"logs\":[");

    struct dirent *de;
    int first = 1, deleted = 0;
    while (off >= 0 && (de = readdir(dir)) != NULL) {
        if (!log_name_ok(de->d_name)) continue;

        char path[512];
        if (snprintf(path, sizeof(path), "%s/%s", log_dir, de->d_name) >= (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) continue;

        if (clear) {
            if (unlink(path) == 0) deleted++;
            continue;
        }
        char time_str[32];
        struct tm modified;
        localtime_r(&st.st_mtime, &modified);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", &modified);

        char name[sizeof(de->d_name) * 2];
        http_json_escape(de->d_name, name, sizeof(name));
        off = http_json_append(json, sizeof(json), off,
            "%s{\"name\":\"%s\",\"size\":%lld,\"mtime\":\"%s\"}",
            first ? "" : ",", name,
            (long long)st.st_size, time_str);
        if (off < 0) break;
        first = 0;
    }
    closedir(dir);

    if (clear) off = snprintf(json, sizeof(json), "{\"deleted\":%d}", deleted);
    else off = http_json_append(json, sizeof(json), off, "]}");
    if (off < 0) http_err(fd, 500, "Error", "Log list response is too large");
    else http_ok_json(fd, json, (size_t)off);
}

/* 下载单个日志文件：复用 http_serve_stream 流式发送 */
static void serve_log_download(int fd, const char *log_dir, const char *name)
{
    if (!log_name_ok(name)) {
        http_handle_404(fd, name);
        return;
    }

    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", log_dir, name) >= (int)sizeof(path)) {
        http_handle_404(fd, name);
        return;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) { http_handle_404(fd, name); return; }

    size_t size = http_file_size(fp);

    char disp[320];
    snprintf(disp, sizeof(disp),
             "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
    http_serve_stream(fd, "application/octet-stream", disp, fp, size, NULL);
}

/* 删除日志文件 */
static void serve_log_delete(int fd, const char *log_dir, const char *name)
{
    if (!log_name_ok(name)) {
        http_err(fd, 400, "Bad Request", NULL);
        return;
    }
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", log_dir, name) >= (int)sizeof(path)) {
        http_err(fd, 400, "Bad Request", NULL);
        return;
    }
    if (unlink(path) == 0)
        http_ok_text(fd, "ok");
    else
        http_err(fd, 404, "Not Found", NULL);
}

/* 日志总入口 */
void http_logs_handler(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)req_buf;
    /* 与核心日志共用配置的日志目录（默认 PATH_LOGS），避免配置 log_dir 后
       Web 端仍读取旧路径导致"看不到日志" */
    const char *log_dir = (app && app->cfg && app->cfg->log_dir[0])
                          ? app->cfg->log_dir : PATH_LOGS;
    if (strcmp(uri, "/logs") == 0 || strcmp(uri, "/logs.html") == 0)
        http_serve_file(fd, "/logs.html");
    else if (strcmp(uri, "/api/logs") == 0)
        serve_logs(fd, log_dir, 0);
    else if (strcmp(uri, "/api/logs/clear") == 0)
        serve_logs(fd, log_dir, 1);
    else if (strncmp(uri, "/logfile/", 9) == 0) {
        char rel[512];
        http_url_decode(uri + 9, rel, sizeof(rel));
        if (strcmp(method, "DELETE") == 0)
            serve_log_delete(fd, log_dir, rel);
        else
            serve_log_download(fd, log_dir, rel);
    }
}
