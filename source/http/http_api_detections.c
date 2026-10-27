/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "http/http_internal.h"
#include "yolo/yolo_events.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DETECTION_LIST_MAX 200

typedef struct {
    char name[64];
    off_t size, json_size;
    struct timespec mtime;
    int objects, focus_valid, focus, autofocus;
} detection_entry_t;

static int date_valid(const char *date)
{
    if (strlen(date) != 8) return 0;
    for (int i = 0; i < 8; i++) if (date[i] < '0' || date[i] > '9') return 0;
    return 1;
}

static const char *json_field(const char *json, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    return p ? p + strlen(pattern) : NULL;
}

static int parse_int_field(const char *json, const char *key, int *out)
{
    const char *p = json_field(json, key);
    if (!p || (*p != '-' && (*p < '0' || *p > '9'))) return -1;
    char *end;
    errno = 0;
    long value = strtol(p, &end, 10);
    if (errno || end == p || value < -2147483647L - 1L || value > 2147483647L ||
        (*end != ',' && *end != '}')) return -1;
    *out = (int)value;
    return 0;
}

static int newest_first(const void *left, const void *right)
{
    const detection_entry_t *a = left, *b = right;
    if (a->mtime.tv_sec != b->mtime.tv_sec) return a->mtime.tv_sec > b->mtime.tv_sec ? -1 : 1;
    if (a->mtime.tv_nsec != b->mtime.tv_nsec) return a->mtime.tv_nsec > b->mtime.tv_nsec ? -1 : 1;
    return strcmp(b->name, a->name);
}

static int read_entry(int day, const char *date, const char *filename, detection_entry_t *entry)
{
    char rel[64];
    if (snprintf(rel, sizeof(rel), "%s/%s", date, filename) >= (int)sizeof(rel) ||
        !yolo_event_rel_valid(rel)) return -1;
    size_t len = strlen(filename);
    if (len < 5 || strcmp(filename + len - 5, ".json")) return -1;
    int metadata_fd = openat(day, filename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (metadata_fd < 0) return -1;
    struct stat json_stat, jpg_stat;
    char jpg[64];
    snprintf(jpg, sizeof(jpg), "%.*s.jpg", (int)(len - 5), filename);
    int rc = -1;
    if (fstat(metadata_fd, &json_stat) || !S_ISREG(json_stat.st_mode) ||
        json_stat.st_size < 2 || json_stat.st_size > 32768 ||
        fstatat(day, jpg, &jpg_stat, AT_SYMLINK_NOFOLLOW) || !S_ISREG(jpg_stat.st_mode)) goto done;
    char metadata[1024];
    ssize_t n = read(metadata_fd, metadata, sizeof(metadata) - 1);
    if (n <= 0) goto done;
    metadata[n] = '\0';
    memset(entry, 0, sizeof(*entry));
    if (parse_int_field(metadata, "object_count", &entry->objects) ||
        entry->objects < 1 || entry->objects > YOLO_MAX_DETS) goto done;
    entry->focus_valid = parse_int_field(metadata, "focus_position_raw", &entry->focus) == 0;
    entry->autofocus = -1;
    const char *autofocus = json_field(metadata, "autofocus");
    if (autofocus && strncmp(autofocus, "true,", 5) == 0) entry->autofocus = 1;
    if (autofocus && strncmp(autofocus, "false,", 6) == 0) entry->autofocus = 0;
    snprintf(entry->name, sizeof(entry->name), "%s/%.*s", date, (int)(len - 5), filename);
    entry->mtime = json_stat.st_mtim;
    entry->size = jpg_stat.st_size;
    entry->json_size = json_stat.st_size;
    rc = 0;
done:
    close(metadata_fd);
    return rc;
}

void http_detections_list(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    int root_fd = open(YOLO_EVENTS_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0) { http_ok_json(fd, "{\"files\":[],\"limit\":200}", 24); return; }
    DIR *root = fdopendir(root_fd);
    if (!root) { close(root_fd); http_err(fd, 500, "Error", NULL); return; }
    detection_entry_t entries[DETECTION_LIST_MAX];
    int count = 0;
    struct dirent *date;
    while ((date = readdir(root))) {
        if (!date_valid(date->d_name)) continue;
        int day_fd = openat(root_fd, date->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (day_fd < 0) continue;
        DIR *day = fdopendir(day_fd);
        if (!day) { close(day_fd); continue; }
        struct dirent *file;
        while ((file = readdir(day))) {
            detection_entry_t candidate;
            if (read_entry(day_fd, date->d_name, file->d_name, &candidate)) continue;
            if (count < DETECTION_LIST_MAX) entries[count++] = candidate;
            else {
                int oldest = 0;
                for (int i = 1; i < count; i++)
                    if (newest_first(&entries[i], &entries[oldest]) > 0) oldest = i;
                if (newest_first(&candidate, &entries[oldest]) < 0) entries[oldest] = candidate;
            }
        }
        closedir(day);
    }
    closedir(root);
    qsort(entries, (size_t)count, sizeof(entries[0]), newest_first);
    char json[65536];
    int off = snprintf(json, sizeof(json), "{\"files\":[");
    for (int i = 0; i < count; i++) {
        detection_entry_t *entry = &entries[i];
        char focus[32] = "null";
        if (entry->focus_valid) snprintf(focus, sizeof(focus), "%d", entry->focus);
        off = http_json_append(json, sizeof(json), off,
            "%s{\"name\":\"%s\",\"date\":\"%.4s-%.2s-%.2s %.2s:%.2s:%.2s\","
            "\"size\":%lld,\"json_size\":%lld,\"objects\":%d,\"focus_position_raw\":%s,\"autofocus\":%s}",
            i ? "," : "", entry->name, entry->name, entry->name + 4, entry->name + 6,
            entry->name + 13, entry->name + 15, entry->name + 17,
            (long long)entry->size, (long long)entry->json_size, entry->objects, focus,
            entry->autofocus < 0 ? "null" : entry->autofocus ? "true" : "false");
        if (off < 0) { http_err(fd, 500, "Error", NULL); return; }
    }
    off = http_json_append(json, sizeof(json), off, "],\"limit\":%d}", DETECTION_LIST_MAX);
    if (off < 0) { http_err(fd, 500, "Error", NULL); return; }
    http_ok_json(fd, json, (size_t)off);
}

void http_detection_file(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req)
{
    (void)app; (void)method; (void)req;
    const char *prefix = "/detectionfile/";
    if (strncmp(uri, prefix, strlen(prefix)) || !yolo_event_rel_valid(uri + strlen(prefix))) {
        http_handle_404(fd, uri); return;
    }
    const char *relative = uri + strlen(prefix), *name = relative + 9;
    char date[9]; memcpy(date, relative, 8); date[8] = '\0';
    int root_fd = open(YOLO_EVENTS_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0) { http_handle_404(fd, uri); return; }
    int day_fd = openat(root_fd, date, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(root_fd);
    if (day_fd < 0) { http_handle_404(fd, uri); return; }
    /* Both downloads require the JSON completion marker and a regular JPEG. */
    size_t len = strlen(name);
    int is_json = len >= 5 && strcmp(name + len - 5, ".json") == 0;
    size_t base_len = len - (is_json ? 5 : 4);
    char marker[64], jpeg[64];
    snprintf(marker, sizeof(marker), "%.*s.json", (int)base_len, name);
    snprintf(jpeg, sizeof(jpeg), "%.*s.jpg", (int)base_len, name);
    struct stat statbuf;
    int valid_pair = !fstatat(day_fd, marker, &statbuf, AT_SYMLINK_NOFOLLOW) && S_ISREG(statbuf.st_mode) &&
                     !fstatat(day_fd, jpeg, &statbuf, AT_SYMLINK_NOFOLLOW) && S_ISREG(statbuf.st_mode);
    int src_fd = valid_pair ? openat(day_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) : -1;
    close(day_fd);
    if (src_fd < 0) { http_handle_404(fd, uri); return; }
    if (fstat(src_fd, &statbuf) || !S_ISREG(statbuf.st_mode) || statbuf.st_size < 0) {
        close(src_fd); http_handle_404(fd, uri); return;
    }
    FILE *source = fdopen(src_fd, "rb");
    if (!source) { close(src_fd); http_err(fd, 500, "Error", NULL); return; }
    char disposition[160];
    snprintf(disposition, sizeof(disposition), "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
    http_serve_stream(fd, is_json ? "application/json" : "image/jpeg", disposition,
                      source, (size_t)statbuf.st_size, NULL);
}
