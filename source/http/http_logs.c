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

/* 解析日志相对路径；公共拆分器拒绝目录穿越和多级路径。 */
static int logs_resolve_rel(const char *rel, char *subdir, size_t subdir_size,
                            char *name, size_t name_size)
{
    return http_split_relative(rel, subdir, subdir_size, name, name_size);
}

/* 将数据完整写入非阻塞 socket（处理 EAGAIN/EWOULDBLOCK 与部分写入）
   复用 http.c 的公共 http_write_all */

/* 日志文件列表 JSON API */
static void serve_log_list_json(int fd, const char *log_dir)
{
    DIR *dir = opendir(log_dir);
    if (!dir) {
        http_err(fd, 500, "Error", NULL);
        return;
    }

    char json[32768];
    int off = snprintf(json, sizeof(json), "{\"logs\":[");

    struct dirent *de;
    int first = 1;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char subdir[512];
        snprintf(subdir, sizeof(subdir), "%s/%s", log_dir, de->d_name);
        struct stat dst;
        if (stat(subdir, &dst) < 0 || !S_ISDIR(dst.st_mode)) continue;

        DIR *sd = opendir(subdir);
        if (!sd) continue;
        struct dirent *fde;
        while ((fde = readdir(sd)) != NULL) {
            if (fde->d_name[0] == '.') continue;

            char path[512];
            snprintf(path, sizeof(path), "%s/%s", subdir, fde->d_name);
            struct stat st;
            if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) continue;

            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M",
                     localtime(&st.st_mtime));

            off = http_json_append(json, sizeof(json), off,
                "%s{\"name\":\"%s/%s\",\"size\":%lld,\"mtime\":\"%s\"}",
                first ? "" : ",", de->d_name, fde->d_name,
                (long long)st.st_size, time_str);
            if (off < 0) break;
            first = 0;
        }
        closedir(sd);
    }
    closedir(dir);

    off = http_json_append(json, sizeof(json), off, "]}");
    http_ok_json(fd, json, (size_t)off);
}

/* 下载单个日志文件：复用 http_serve_stream 流式发送 */
static void serve_log_download(int fd, const char *log_dir, const char *rel)
{
    char subdir[64], name[256];
    if (logs_resolve_rel(rel, subdir, sizeof(subdir), name, sizeof(name)) != 0) {
        http_handle_404(fd, rel);
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", log_dir, subdir, name);

    FILE *fp = fopen(path, "rb");
    if (!fp) { http_handle_404(fd, rel); return; }

    size_t size = http_file_size(fp);

    char disp[320];
    snprintf(disp, sizeof(disp),
             "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
    http_serve_stream(fd, "application/octet-stream", disp, fp, size, NULL);
}

/* 删除日志文件 */
static void serve_log_delete(int fd, const char *log_dir, const char *rel)
{
    char subdir[64], name[256];
    if (logs_resolve_rel(rel, subdir, sizeof(subdir), name, sizeof(name)) != 0) {
        http_err(fd, 400, "Bad Request", NULL);
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", log_dir, subdir, name);
    if (unlink(path) == 0)
        http_ok_text(fd, "ok");
    else
        http_err(fd, 404, "Not Found", NULL);
}

/* 一键清空日志：删除全部日志文件。
   当前进程仍持有旧文件句柄，log.c 的 rotate_check 会在下次写入时
   检测到文件被 unlink 并自动重建，日志不中断 */
static void serve_log_clear(int fd, const char *log_dir)
{
    int del = 0;
    DIR *dir = opendir(log_dir);
    if (!dir) { http_err(fd, 500, "Error", NULL); return; }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char subdir[512];
        snprintf(subdir, sizeof(subdir), "%s/%s", log_dir, de->d_name);
        struct stat dst;
        if (stat(subdir, &dst) < 0 || !S_ISDIR(dst.st_mode)) continue;
        DIR *sd = opendir(subdir);
        if (!sd) continue;
        struct dirent *fde;
        while ((fde = readdir(sd)) != NULL) {
            if (fde->d_name[0] == '.') continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", subdir, fde->d_name);
            struct stat st;
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && unlink(path) == 0)
                del++;
        }
        closedir(sd);
    }
    closedir(dir);
    char json[64];
    int off = snprintf(json, sizeof(json), "{\"deleted\":%d}", del);
    http_ok_json(fd, json, (size_t)off);
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
        serve_log_list_json(fd, log_dir);
    else if (strcmp(uri, "/api/logs/clear") == 0)
        serve_log_clear(fd, log_dir);
    else if (strncmp(uri, "/logfile/", 9) == 0) {
        char rel[512];
        http_url_decode(uri + 9, rel, sizeof(rel));
        if (strcmp(method, "DELETE") == 0)
            serve_log_delete(fd, log_dir, rel);
        else
            serve_log_download(fd, log_dir, rel);
    }
}
