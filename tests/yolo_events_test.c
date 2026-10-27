/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise actual async persistence and HTTP pair visibility, without a camera. */
#include "yolo/yolo_events.c"
#include "http/http_api_detections.c"
#include <assert.h>

static char response[65536];
static size_t response_len;
static int response_code;

void _log_error(const char *file, int line, const char *fmt, ...)
{ (void)file; (void)line; (void)fmt; }

void http_ok_json(int fd, const char *json, size_t len)
{
    (void)fd;
    assert(len < sizeof(response));
    memcpy(response, json, len); response[len] = '\0';
    response_len = len; response_code = 200;
}

void http_err(int fd, int code, const char *status, const char *message)
{ (void)fd; (void)status; (void)message; response_code = code; response[0] = '\0'; }

void http_handle_404(int fd, const char *uri)
{ (void)uri; http_err(fd, 404, "Not found", NULL); }

void http_serve_stream(int fd, const char *mime, const char *headers,
                       FILE *file, size_t size, const char *unlink_after)
{
    (void)fd; (void)mime; (void)unlink_after;
    assert(strstr(headers, "Content-Disposition: attachment;"));
    assert(size < sizeof(response));
    response_len = fread(response, 1, size, file);
    assert(response_len == size);
    response[response_len] = '\0'; response_code = 200;
    fclose(file);
}

int http_json_append(char *json, size_t size, int off, const char *fmt, ...)
{
    if (off < 0 || (size_t)off >= size) return -1;
    va_list args; va_start(args, fmt);
    int n = vsnprintf(json + off, size - (size_t)off, fmt, args);
    va_end(args);
    return n < 0 || (size_t)n >= size - (size_t)off ? -1 : off + n;
}

static void submit_until_accepted(const unsigned char *jpeg, size_t size, yolo_event_meta_t *meta)
{
    for (int i = 0; i < 1000; i++) {
        if (yolo_events_submit(jpeg, size, meta) == 0) return;
        usleep(1000);
    }
    assert(!"writer never accepted snapshot");
}

static void get_snapshot(const char *relative, const char *suffix)
{
    char uri[128];
    snprintf(uri, sizeof(uri), "/detectionfile/%s.%s", relative, suffix);
    http_detection_file(NULL, 0, "GET", uri, NULL);
}

int main(void)
{
    assert(yolo_event_rel_valid("20261027/det_010203_004_1.jpg"));
    assert(yolo_event_rel_valid("20261027/det_010203_004_18446744073709551615.json"));
    const char *invalid[] = {"../det_010203_004_1.jpg", "20261027/../secret.jpg",
        "20261027/det_010203_004_1.jpg/extra", "20261027/det_010203_004_1.jpg?x=1",
        "20261027/det_010203_004_.json", "20261027/det_010203_004_1.png",
        "2026102a/det_010203_004_1.json", "20261027/det_010203_004_111111111111111111111.json"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(!yolo_event_rel_valid(invalid[i]));
    assert(!yolo_events_due(1, 1000));
    unsigned char jpeg[] = {0xff, 0xd8, 0x12, 0x34, 0xff, 0xd9};
    yolo_event_meta_t meta = {
        .seq = 7, .capture_monotonic_ms = 5000, .capture_unix_ms = 1793030400123ull,
        .width = 1280, .height = 720, .focus_valid = 0, .focus_position = 999,
        .autofocus = -1, .focus_sample_monotonic_ms = 4900, .count = 1,
        .objects = {{.cls = 0, .name = "target\"\\\n", .conf = .75f,
                     .x1 = 10, .y1 = 20, .x2 = 110, .y2 = 220}}
    };
    /* Full and contended queues return immediately, without a writer or I/O. */
    atomic_store(&events.enabled, 1); events.count = YOLO_EVENT_QUEUE_SIZE;
    assert(yolo_events_submit(jpeg, sizeof(jpeg), &meta) == -1);
    events.count = 0;
    pthread_mutex_lock(&events.lock);
    assert(yolo_events_submit(jpeg, sizeof(jpeg), &meta) == -1);
    pthread_mutex_unlock(&events.lock);
    atomic_store(&events.enabled, 0);

    char temp[] = "/tmp/yolo-events-test-XXXXXX";
    assert(mkdtemp(temp));
    int original_dir = open(".", O_RDONLY | O_DIRECTORY);
    assert(original_dir >= 0 && chdir(temp) == 0);
    assert(yolo_events_init(NULL) == 0);
    assert(yolo_events_init(NULL) == 0);
    assert(!yolo_events_due(0, 1000));
    assert(yolo_events_due(1, 1000));
    assert(!yolo_events_due(1, 1000));
    assert(!yolo_events_due(1, 1999));
    assert(yolo_events_due(1, 2000));
    assert(!yolo_events_due(1, 1999));
    assert(!yolo_events_due(1, ULLONG_MAX));
    assert(yolo_events_submit(jpeg, YOLO_EVENT_JPEG_MAX + 1u, &meta) == -1);
    yolo_event_meta_t bad = meta;
    bad.objects[0].conf = NAN;
    assert(yolo_events_submit(jpeg, sizeof(jpeg), &bad) == -1);
    submit_until_accepted(jpeg, sizeof(jpeg), &meta);
    meta.seq = 8; meta.focus_valid = 1; meta.focus_position = 220; meta.autofocus = 1;
    meta.focus_sample_monotonic_ms = 6000; /* Future sample: age must be unknown. */
    submit_until_accepted(jpeg, sizeof(jpeg), &meta);
    memset(jpeg, 0, sizeof(jpeg)); memset(&meta, 0, sizeof(meta));
    yolo_events_destroy();
    assert(!yolo_events_due(1, 9000));
    assert(yolo_events_submit(jpeg, sizeof(jpeg), &meta) == -1);
    yolo_events_destroy();

    int root = open(YOLO_EVENTS_DIR, O_RDONLY | O_DIRECTORY);
    assert(root >= 0);
    DIR *dirs = fdopendir(dup(root));
    assert(dirs);
    char date[9] = "";
    struct dirent *dir;
    while ((dir = readdir(dirs))) if (date_valid(dir->d_name)) memcpy(date, dir->d_name, sizeof(date));
    closedir(dirs); assert(date[0]);
    int day = openat(root, date, O_RDONLY | O_DIRECTORY);
    assert(day >= 0);
    DIR *files = fdopendir(dup(day)); assert(files);
    char relative7[64] = "", relative8[64] = "";
    int completed = 0, images = 0;
    struct dirent *file;
    while ((file = readdir(files))) {
        if (file->d_name[0] == '.') { assert(!strstr(file->d_name, ".tmp")); continue; }
        size_t n = strlen(file->d_name);
        if (n > 5 && strcmp(file->d_name + n - 5, ".json") == 0) {
            completed++;
            char *destination = strstr(file->d_name, "_7.json") ? relative7 : relative8;
            snprintf(destination, 64, "%s/%.*s", date, (int)n - 5, file->d_name);
        } else if (n > 4 && strcmp(file->d_name + n - 4, ".jpg") == 0) images++;
    }
    closedir(files);
    assert(completed == 2 && images == 2 && relative7[0] && relative8[0]);
    get_snapshot(relative7, "json"); assert(response_code == 200);
    assert(strstr(response, "\"seq\":7,"));
    assert(strstr(response, "\"capture_unix_estimated\":true,"));
    assert(strstr(response, "\"focus_position_raw\":null,"));
    assert(strstr(response, "\"focus_unit\":\"device_units\","));
    assert(strstr(response, "\"focus_sample_age_ms\":100,"));
    assert(strstr(response, "\"autofocus\":null,"));
    assert(strstr(response, "\"focal_length_mm\":null,"));
    assert(strstr(response, "\"name\":\"target\\\"\\\\\\u000a\""));
    assert(strstr(response, "\"bbox\":[10.000,20.000,110.000,220.000]"));
    get_snapshot(relative8, "json"); assert(response_code == 200);
    assert(strstr(response, "\"focus_position_raw\":220,"));
    assert(strstr(response, "\"focus_sample_age_ms\":null,"));
    assert(strstr(response, "\"autofocus\":true,"));
    get_snapshot(relative7, "jpg"); assert(response_code == 200 && response_len == sizeof(jpeg));
    assert((unsigned char)response[0] == 0xff && (unsigned char)response[2] == 0x12);

    /* Orphan images and symlinks are never listed or downloadable. */
    const char *orphan = "det_010203_004_999.jpg";
    int orphan_fd = openat(day, orphan, O_WRONLY | O_CREAT | O_EXCL, 0644);
    assert(orphan_fd >= 0); assert(write(orphan_fd, "orphan", 6) == 6); close(orphan_fd);
    char orphan_rel[64]; snprintf(orphan_rel, sizeof(orphan_rel), "%s/det_010203_004_999", date);
    get_snapshot(orphan_rel, "jpg"); assert(response_code == 404);
    assert(symlinkat("/etc/passwd", day, "det_010203_004_999.json") == 0);
    get_snapshot(orphan_rel, "json"); assert(response_code == 404);
    http_detection_file(NULL, 0, "GET", "/detectionfile/20261027/../secret.json", NULL);
    assert(response_code == 404);
    http_detections_list(NULL, 0, "GET", "/api/detections", NULL);
    assert(response_code == 200);
    assert(strstr(response, relative7) && strstr(response, relative8));
    assert(!strstr(response, "999"));
    assert(strstr(response, "\"objects\":1,"));
    assert(strstr(response, "\"focus_position_raw\":null,"));
    assert(strstr(response, "\"focus_position_raw\":220,"));

    files = fdopendir(dup(day)); assert(files); rewinddir(files);
    while ((file = readdir(files))) if (file->d_name[0] != '.') assert(unlinkat(day, file->d_name, 0) == 0);
    closedir(files); close(day);
    assert(unlinkat(root, date, AT_REMOVEDIR) == 0); close(root);
    assert(rmdir(YOLO_EVENTS_DIR) == 0 && rmdir("recordings") == 0);
    assert(fchdir(original_dir) == 0); close(original_dir); assert(rmdir(temp) == 0);
    puts("PASS: async snapshots copy inputs, bound queues, throttle, drain, publish pairs, preserve focus nulls and protect downloads");
    return 0;
}
