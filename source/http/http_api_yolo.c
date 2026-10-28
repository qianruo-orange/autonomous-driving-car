/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_yolo.c — YOLO 文件上传接口：POST /api/yolo/upload?type=model|names
 *
 * 请求 body 为原始文件内容（前端 fetch 直接以二进制上传，不用 multipart）。
 * HTTP 层（http.c）在解析请求头后把 body 流式落盘到 /tmp 临时文件，
 * 处理完成后把临时路径传给本接口；本接口负责：
 *   1) 校验（模型：RKNN magic + 大小；标签：文本格式 + 行数/长度）
 *   2) 模型原子替换 config/yolo26.rknn；标签保存在 config/
 *   3) 更新运行时配置并落盘 config.txt
 *   4) 热重载推理池 rknn_yolo_reload()，无需重启服务
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "http/http_internal.h"
#include "yolo/rknn_yolo.h"
#include "yolo/yolo_draw.h"     /* yolo_classes_t / YOLO_MAX_CLASSES / YOLO_CLASS_NAME_LEN */
#include "core/common.h"      /* safe_strncpy */

#define YOLO_MODEL_MAX_SIZE  (64 * 1024 * 1024)   /* 模型文件上限 64MB */
#define YOLO_NAMES_MAX_SIZE  (256 * 1024)         /* 类别标签文件上限 256KB */

/* 从路径取 basename（拒绝含 '/' 或空的 basename，防止路径逃逸） */
static const char *yolo_safe_basename(const char *path, const char *fallback)
{
    if (!path || !path[0]) return fallback;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!base[0] || strchr(base, '/') || strlen(base) > 64) return fallback;
    return base;
}

/* The caller supplies a path generated from the fixed platform table. Reject
 * symlinked parent components instead of following them during uploads. */
static int yolo_prepare_model_directory(const char *path)
{
    char directory[320];
    safe_strncpy(directory, sizeof(directory), path);
    char *file = strrchr(directory, '/');
    if (!file) return -1;
    *file = '\0';
    for (char *p = directory; ; ++p) {
        if (*p && *p != '/') continue;
        char saved = *p;
        *p = '\0';
        if (mkdir(directory, 0755) < 0 && errno != EEXIST) return -1;
        struct stat status;
        if (lstat(directory, &status) < 0 || !S_ISDIR(status.st_mode)) return -1;
        *p = saved;
        if (!saved) break;
    }
    return 0;
}

/* Upload staging may live on /tmp (a different filesystem). Copy into a
 * sibling temporary file and rename there so replacement stays atomic. */
static int yolo_atomic_install(const char *source, const char *destination)
{
    char temporary[352];
    if (snprintf(temporary, sizeof(temporary), "%s.upload-XXXXXX", destination) >= (int)sizeof(temporary)) return -1;
    int output = mkstemp(temporary);
    if (output < 0) return -1;
    FILE *input = fopen(source, "rb");
    int failed = !input;
    unsigned char buffer[16384];
    while (!failed) {
        size_t length = fread(buffer, 1, sizeof(buffer), input);
        if (!length) { failed = ferror(input); break; }
        size_t offset = 0;
        while (offset < length) {
            ssize_t written = write(output, buffer + offset, length - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { failed = 1; break; }
            offset += (size_t)written;
        }
    }
    if (input && fclose(input)) failed = 1;
    if (!failed && (fchmod(output, 0644) || fsync(output))) failed = 1;
    if (close(output)) failed = 1;
    if (!failed && rename(temporary, destination)) failed = 1;
    if (failed) unlink(temporary);
    else unlink(source);
    return failed ? -1 : 0;
}

/* 校验类别标签文件：每行一个类名（1~128 行，每行 ≤23 字符，允许空行），
   全部满足返回行数，否则 -1 */
static int yolo_validate_names(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int n = 0;
    char line[YOLO_CLASS_NAME_LEN + 8];
    while (fgets(line, sizeof(line), fp)) {
        char *s = line, *e = line + strlen(line);
        while (s < e && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
        char *t = e;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r' || t[-1] == '\n')) t--;
        if (t == s) continue;   /* 空行允许 */
        if (t - s > YOLO_CLASS_NAME_LEN - 1) { fclose(fp); return -1; }
        n++;
        if (n > YOLO_MAX_CLASSES) { fclose(fp); return -1; }
    }
    int err = ferror(fp);
    fclose(fp);
    return (err || n == 0) ? -1 : n;
}

void http_yolo_upload(app_ctx_t *app, int fd, const char *uri, const char *tmp_path)
{
    if (!app || !app->cfg || !uri || !tmp_path || !tmp_path[0]) {
        http_err(fd, 400, "Bad Request", NULL);
        return;
    }

    /* 从 query string 解析上传类型 */
    char type[16] = {0};
    const char *q = strchr(uri, '?');
    if (!q || !http_form_get_param(q, "type=", type, sizeof(type)) ||
        (strcmp(type, "model") != 0 && strcmp(type, "names") != 0)) {
        http_err(fd, 400, "Bad Request", "bad type");
        return;
    }
    int is_model = (strcmp(type, "model") == 0);

    /* 大小与内容校验 */
    FILE *fp = fopen(tmp_path, "rb");
    if (!fp) { http_err(fd, 500, "Error", "cannot read upload"); return; }
    size_t sz = http_file_size(fp);
    if (sz == 0) {
        fclose(fp);
        http_err(fd, 400, "Bad Request", "empty file");
        return;
    }
    if (is_model) {
        if (sz > YOLO_MODEL_MAX_SIZE) {
            fclose(fp);
            http_err(fd, 413, "Payload Too Large", "model too large");
            return;
        }
        unsigned char magic[4] = {0};
        int bad = (fread(magic, 1, 4, fp) != 4) ||
                  memcmp(magic, "RKNN", 4) != 0;
        fclose(fp);
        if (bad) {
            http_err(fd, 400, "Bad Request", "invalid rknn model (bad magic)");
            return;
        }
    } else {
        fclose(fp);
        if (sz > YOLO_NAMES_MAX_SIZE) {
            http_err(fd, 413, "Payload Too Large", "names too large");
            return;
        }
        if (yolo_validate_names(tmp_path) < 0) {
            http_err(fd, 400, "Bad Request", "invalid names file");
            return;
        }
    }

    /* Model conversion and uploads use the same runtime path.
       Labels retain their safe basename under config/. */
    char path[sizeof(app->cfg->yolo_model)];
    if (is_model) {
        rknn_yolo_default_model_path(path, sizeof(path));
        if (yolo_prepare_model_directory(path) < 0) {
            http_err(fd, 500, "Error", "cannot prepare model directory");
            return;
        }
    } else {
        snprintf(path, sizeof(path), "config/%s", yolo_safe_basename(app->cfg->yolo_names, "coco.names"));
    }

    /* 校验通过：原子替换正式文件 */
    if (yolo_atomic_install(tmp_path, path) != 0) {
        remove(tmp_path);
        http_err(fd, 500, "Error", "cannot write file");
        return;
    }

    /* 更新运行时配置并落盘 */
    if (is_model)
        safe_strncpy(app->cfg->yolo_model, sizeof(app->cfg->yolo_model), path);
    else
        safe_strncpy(app->cfg->yolo_names, sizeof(app->cfg->yolo_names), path);
    config_save(app);

    /* 热重载推理池：加载新模型/新标签，无需重启服务 */
    int rc = rknn_yolo_reload();

    char msg[512];
    if (is_model) {
        snprintf(msg, sizeof(msg),
                 "{\"result\":\"%s\",\"type\":\"model\",\"path\":\"%s\",\"size\":%zu}",
                 rc == 0 ? "ok" : "saved_reload_failed", path, sz);
    } else {
        snprintf(msg, sizeof(msg),
                 "{\"result\":\"%s\",\"type\":\"names\",\"path\":\"%s\",\"size\":%zu,\"classes\":%d}",
                 rc == 0 ? "ok" : "saved_reload_failed", path, sz, yolo_validate_names(path));
    }
    http_ok_json(fd, msg, strlen(msg));
}
