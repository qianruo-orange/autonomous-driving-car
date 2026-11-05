/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_rec.c — 网络录像控制路由：
 *   POST /api/rec/start   开始录制（YOLO 画框帧优先）
 *   POST /api/rec/stop    停止录制并 finalize
 *   GET  /api/rec/status  录制状态
 *   GET  /api/rec/list    录制文件列表
 *   GET  /api/rec/pack    打包下载全部录像 (tar.gz)
 *   POST /api/rec/delete  删除录制文件（body = 文件名）
 *   GET  /recfile/<name>  下载录制文件（recordings/videos/ 一级平铺）
 */

#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include "http/http_internal.h"
#include "video/video_rec.h"

#define REC_DIR PATH_VIDEOS

/* 单文件名合法性：video_YYYYMMDD.mp4（同一天重开时带 _N 后缀）；不含路径分隔符 */
static int rec_file_valid(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len < 6 + 8 + 4 || strncmp(name, "video_", 6) ||
        strcmp(name + len - 4, ".mp4"))
        return 0;
    for (size_t i = 6; i + 4 < len; i++)
        if (name[i] != '_' && (name[i] < '0' || name[i] > '9'))
            return 0;
    return 1;
}

/* 状态 JSON */
static void serve_rec_status(int fd)
{
    video_rec_status_t st;
    char json[768];
    int off;
    video_rec_status(&st);
    off = snprintf(json, sizeof(json),
        "{\"recording\":%d,\"file\":\"%s\",\"start_ms\":%llu,"
        "\"frames\":%llu,\"bytes\":%llu,\"fps\":%.1f,"
        "\"split_pending\":%d,\"split_error\":%d,"
        "\"skipped_cadence\":%llu,\"skipped_size\":%llu,"
        "\"io_error\":%d,\"free_mb\":%llu}",
        st.recording, st.file[0] ? st.file : "",
        (unsigned long long)st.start_ms,
        (unsigned long long)st.frames, (unsigned long long)st.bytes, st.fps,
        st.split_pending, st.split_error,
        st.skipped_cadence, st.skipped_size, st.io_error,
        st.free_bytes >> 20);
    http_ok_json(fd, json, (size_t)off);
}

/* 录制文件列表（按 mtime 倒序；recordings/videos/ 一级平铺） */
static void serve_rec_list(int fd)
{
    struct ent { char name[160]; time_t mtime; int64_t size; };
    static struct ent ents[1024];
    int n = 0;
    int cap = (int)(sizeof(ents)/sizeof(ents[0]));

    DIR *dir = opendir(REC_DIR);
    if (!dir) {
        http_err(fd, 500, "Error", NULL);
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n < cap) {
        if (de->d_name[0] == '.') continue;
        if (!rec_file_valid(de->d_name)) continue;
        struct stat st;
        char path[512];
        if (snprintf(path, sizeof(path), "%s/%s", REC_DIR, de->d_name) >= (int)sizeof(path)) continue;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        safe_strncpy(ents[n].name, sizeof(ents[n].name), de->d_name);
        ents[n].mtime = st.st_mtime;
        ents[n].size  = (int64_t)st.st_size;
        n++;
    }
    closedir(dir);

    /* mtime 倒序：新录制在前 */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (ents[j].mtime > ents[i].mtime) {
                struct ent t = ents[i]; ents[i] = ents[j]; ents[j] = t;
            }

    char json[65536];
    int off = snprintf(json, sizeof(json), "{\"files\":[");
    for (int i = 0; i < n; i++) {
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&ents[i].mtime));
        off = http_json_append(json, sizeof(json), off,
            "%s{\"name\":\"%s\",\"size\":%lld,\"mtime\":\"%s\"}",
            i ? "," : "", ents[i].name, (long long)ents[i].size, ts);
        if (off < 0) break;
    }
    off = http_json_append(json, sizeof(json), off, "]}");
    http_ok_json(fd, json, (size_t)off);
}

/* 下载：/recfile/<video_YYYYMMDD.mp4> */
static void serve_rec_download(int fd, const char *name)
{
    if (!rec_file_valid(name)) {
        http_handle_404(fd, name);
        return;
    }
    char path[512];
    if (snprintf(path, sizeof(path), "%s/%s", REC_DIR, name) >= (int)sizeof(path)) {
        http_handle_404(fd, name);
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) { http_handle_404(fd, name); return; }
    size_t size = http_file_size(fp);
    char disp[320];
    snprintf(disp, sizeof(disp),
             "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
    http_serve_stream(fd, "video/mp4", disp, fp, size, NULL);
}

/* 删除：body = video_YYYYMMDD.mp4 */
static void serve_rec_delete(int fd, const char *req)
{
    char name[160];
    /* req 是完整 HTTP 请求，必须提取 body（此前直接当 body 用 → 永远 400） */
    const char *body = http_body_start(req);
    if (!body || !*body) { http_err(fd, 400, "Bad Request", NULL); return; }
    safe_strncpy(name, sizeof(name), body);
    /* 去掉尾部空白/换行 */
    size_t len = strlen(name);
    while (len > 0 && (name[len-1] == '\n' || name[len-1] == '\r' ||
                       name[len-1] == ' ')) name[--len] = '\0';
    if (!rec_file_valid(name)) {
        http_err(fd, 400, "Bad Request", NULL);
        return;
    }
    int root = open(REC_DIR, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    if (root < 0) { http_err(fd, 404, "Not Found", NULL); return; }
    int removed = video_rec_delete_file(root, name);
    int error = errno;
    close(root);
    errno = error;
    if(removed==1)http_err(fd,409,"Conflict","recording in progress");
    else if(!removed)http_ok_text(fd,"ok");
    else http_err(fd,removed==2||errno==ENOENT?404:500,removed==2||errno==ENOENT?"Not Found":"Error",NULL);
}

/* 一键清空录像：删除全部录像文件，保留正在录制的文件 */
static void serve_rec_clear(int fd)
{
    int del = 0, failed = 0;
    int root = open(REC_DIR,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    DIR *dir = root < 0 ? NULL : fdopendir(root);
    if(!dir){if(root>=0)close(root);http_err(fd,500,"Error",NULL);return;}
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!rec_file_valid(de->d_name)) continue;   /* 跳过打包临时文件等 */
        int removed = video_rec_delete_file(dirfd(dir), de->d_name);
        if (!removed) del++;
        else if (removed < 0) failed = 1;
    }
    closedir(dir);
    if(failed){http_err(fd,500,"Error","Some recordings could not be deleted");return;}
    char json[64];
    int off = snprintf(json, sizeof(json), "{\"deleted\":%d}", del);
    http_ok_json(fd, json, (size_t)off);
}

/* 总入口 */
void http_rec_handler(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)app;
    if (strcmp(uri, "/api/rec/status") == 0) {
        serve_rec_status(fd);
    } else if (strcmp(uri, "/api/rec/list") == 0) {
        serve_rec_list(fd);
    } else if (strcmp(uri, "/api/rec/start") == 0) {
        if (strcmp(method, "POST") != 0) { http_err(fd, 405, "Method Not Allowed", NULL); return; }
        if (video_rec_start() != 0)
            http_err(fd, 409, "Conflict", "already recording");
        else
            http_ok_text(fd, "started");
    } else if (strcmp(uri, "/api/rec/stop") == 0) {
        if (strcmp(method, "POST") != 0) { http_err(fd, 405, "Method Not Allowed", NULL); return; }
        if (video_rec_stop() != 0)
            http_err(fd, 409, "Conflict", "not recording");
        else
            http_ok_text(fd, "stopped");
    } else if (strcmp(uri, "/api/rec/split") == 0) {
        if (strcmp(method, "POST") != 0) { http_err(fd, 405, "Method Not Allowed", NULL); return; }
        const char *file = http_body_start(req_buf);
        if (!file || !rec_file_valid(file)) {
            http_err(fd, 400, "Bad Request", "expected recording filename required");
            return;
        }
        int rc = video_rec_split(file);
        if (rc != 0) http_err(fd, 409, "Conflict", rc == -2 ? "recording changed" : "not recording or split pending");
        else http_ok_text(fd, "split queued");
    } else if (strcmp(uri, "/api/rec/pack") == 0) {
        http_err(fd, 405, "Method Not Allowed", NULL);
    } else if (strcmp(uri, "/api/rec/delete") == 0) {
        if (strcmp(method, "POST") != 0) { http_err(fd, 405, "Method Not Allowed", NULL); return; }
        serve_rec_delete(fd, req_buf);
    } else if (strcmp(uri, "/api/rec/clear") == 0) {
        if (strcmp(method, "POST") != 0) { http_err(fd, 405, "Method Not Allowed", NULL); return; }
        serve_rec_clear(fd);
    } else if (strncmp(uri, "/recfile/", 9) == 0) {
        char name[128];
        http_url_decode(uri + 9, name, sizeof(name));
        serve_rec_download(fd, name);
    } else {
        http_handle_404(fd, uri);
    }
}
