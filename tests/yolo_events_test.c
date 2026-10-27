/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise production metadata queue, daily append and protected downloads. */
#include <unistd.h>
#include <sys/types.h>
static ssize_t test_write(int fd, const void *bytes, size_t length);
#define write test_write
#include "yolo/yolo_events.c"
#undef write
#include "http/http_api_detections.c"
#include <assert.h>

static atomic_long write_budget = -1;
static atomic_int write_failures, recovery_logs, shutdown_failure_logs;
static char response[65536], response_mime[64];
static size_t response_len;
static int response_code;

static ssize_t test_write(int fd, const void *bytes, size_t length)
{
    long budget = atomic_load(&write_budget);
    if (budget < 0) return write(fd, bytes, length);
    if (!budget) { atomic_fetch_add(&write_failures, 1); errno = ENOSPC; return -1; }
    if (length > (size_t)budget) length = (size_t)budget;
    ssize_t n = write(fd, bytes, length);
    if (n > 0) atomic_fetch_sub(&write_budget, n);
    return n;
}

void _log_error(const char *file, int line, const char *fmt, ...)
{
    (void)file; (void)line;
    if (strstr(fmt, "shutdown storage failure")) atomic_fetch_add(&shutdown_failure_logs, 1);
}
void _log_info(const char *file, int line, const char *fmt, ...)
{
    (void)file; (void)line;
    if (strstr(fmt, "storage recovered")) atomic_fetch_add(&recovery_logs, 1);
}
void http_ok_json(int fd, const char *json, size_t len)
{
    (void)fd; assert(len < sizeof(response));
    memcpy(response, json, len); response[len] = '\0'; response_len = len; response_code = 200;
}
void http_err(int fd, int code, const char *status, const char *message)
{ (void)fd; (void)status; (void)message; response_code = code; response[0] = '\0'; }
void http_handle_404(int fd, const char *uri)
{ (void)uri; http_err(fd, 404, "Not found", NULL); }
void http_serve_stream(int fd, const char *mime, const char *headers,
                       FILE *file, size_t size, const char *unlink_after)
{
    (void)fd; (void)unlink_after;
    assert(strstr(headers, "Content-Disposition: attachment;"));
    assert(size < sizeof(response));
    snprintf(response_mime, sizeof(response_mime), "%s", mime);
    response_len = fread(response, 1, size, file);
    assert(response_len == size);
    response[response_len] = '\0'; response_code = 200; fclose(file);
}
int http_json_append(char *json, size_t size, int off, const char *fmt, ...)
{
    if (off < 0 || (size_t)off >= size) return -1;
    va_list args; va_start(args, fmt);
    int n = vsnprintf(json + off, size - (size_t)off, fmt, args);
    va_end(args);
    return n < 0 || (size_t)n >= size - (size_t)off ? -1 : off + n;
}

const char *http_body_start(const char *request)
{
    if (!request) return "";
    const char *body = strstr(request, "\r\n\r\n");
    return body ? body + 4 : request;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t done_cond;
    yolo_event_meta_t meta;
    int done, status;
} producer_t;

static void *submit_one(void *argument)
{
    producer_t *producer = argument;
    int status = yolo_events_submit_meta(&producer->meta);
    pthread_mutex_lock(&producer->lock);
    producer->status = status; producer->done = 1;
    pthread_cond_signal(&producer->done_cond);
    pthread_mutex_unlock(&producer->lock);
    return NULL;
}

static int producer_done(producer_t *producer, int milliseconds)
{
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)milliseconds * 1000000L;
    until.tv_sec += until.tv_nsec / 1000000000L; until.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&producer->lock);
    while (!producer->done)
        if (pthread_cond_timedwait(&producer->done_cond, &producer->lock, &until)) break;
    int done = producer->done;
    pthread_mutex_unlock(&producer->lock);
    return done;
}

static void check_backpressure(const yolo_event_meta_t *meta)
{
    producer_t producer = {.lock = PTHREAD_MUTEX_INITIALIZER, .done_cond = PTHREAD_COND_INITIALIZER, .meta = *meta};
    events.count = YOLO_EVENT_QUEUE_SIZE; events.head = 0; events.stopping = 0; events.recent_count = events.recent_next = 0;
    atomic_store(&events.enabled, 1);
    pthread_t thread; assert(pthread_create(&thread, NULL, submit_one, &producer) == 0);
    assert(!producer_done(&producer, 20));
    pthread_mutex_lock(&events.lock); events.count = 0; pthread_cond_signal(&events.space); pthread_mutex_unlock(&events.lock);
    assert(producer_done(&producer, 500) && producer.status == 0);
    pthread_join(thread, NULL);
    assert(events.count == 1); /* Waiting for space did not drop the frame. */

    producer.done = 0; producer.meta.seq++; events.recent_count = events.recent_next = 0;
    pthread_mutex_lock(&events.lock);
    assert(pthread_create(&thread, NULL, submit_one, &producer) == 0);
    assert(!producer_done(&producer, 20));
    pthread_mutex_unlock(&events.lock);
    assert(producer_done(&producer, 500) && producer.status == 0);
    pthread_join(thread, NULL); /* Ordinary mutex contention also waits. */

    producer.done = 0; producer.meta.seq++; events.count = YOLO_EVENT_QUEUE_SIZE; events.recent_count = events.recent_next = 0;
    assert(pthread_create(&thread, NULL, submit_one, &producer) == 0);
    assert(!producer_done(&producer, 20));
    yolo_events_destroy();
    assert(producer_done(&producer, 500) && producer.status == -1);
    pthread_join(thread, NULL);
    events.count = 0; events.stopping = 0; atomic_store(&events.enabled, 0);
    pthread_cond_destroy(&producer.done_cond); pthread_mutex_destroy(&producer.lock);
}

static void submit_until_accepted(const yolo_event_meta_t *meta)
{
    for (int i = 0; i < 1000; i++) {
        if (yolo_events_submit_meta(meta) == 0) return;
        usleep(1000);
    }
    assert(!"writer never accepted metadata");
}
static void get_report(const char *day)
{
    /* Inspect the durable append data directly; standard XLSX export and its
       download/pack routes are covered separately by the bulk/export tests. */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/.detections_%s.rows", YOLO_EVENTS_DIR, day, day);
    FILE *input = fopen(path, "rb");
    if (!input) { response_code = 404; response_len = 0; response[0] = '\0'; return; }
    response_len = fread(response, 1, sizeof(response) - 1, input);
    assert(!ferror(input) && feof(input));
    fclose(input);
    response[response_len] = '\0'; response_code = 200;
    snprintf(response_mime, sizeof(response_mime), "text/plain; charset=utf-8");
}
static int line_count(const char *body)
{
    int lines = 0; for (; *body; ++body) if (*body == '\n') lines++;
    return lines;
}
static int occurrence_count(const char *body, const char *needle)
{
    int count = 0;
    for (const char *p = body; (p = strstr(p, needle)); p += strlen(needle)) count++;
    return count;
}
static void clear_day(int root, const char *day)
{
    int directory = openat(root, day, O_RDONLY | O_DIRECTORY | O_NOFOLLOW); assert(directory >= 0);
    DIR *files = fdopendir(dup(directory)); assert(files);
    struct dirent *file;
    while ((file = readdir(files))) if (strcmp(file->d_name, ".") && strcmp(file->d_name, "..")) assert(unlinkat(directory, file->d_name, 0) == 0);
    closedir(files); close(directory); assert(unlinkat(root, day, AT_REMOVEDIR) == 0);
}

int main(void)
{
    setenv("TZ", "Asia/Shanghai", 1); tzset();
    assert(yolo_report_rel_valid("20261027/detections_20261027.xlsx"));
    const char *invalid[] = {"20261027/detections_20261028.xlsx", "20261027/../secret.txt",
        "2026102a/detections_2026102a.xlsx", "20261027/detections_20261027.xlsx/extra",
        "20261027/detections_20261027.xlsx?x=1", "20261027/random.txt"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) assert(!yolo_report_rel_valid(invalid[i]));
    assert(yolo_event_rel_valid("20261027/det_010203_004_1.jpg"));
    assert(!yolo_events_due(1, 1000));
    yolo_event_meta_t meta = {.seq = 7, .capture_unix_ms = 1793030400123ull,
        .width = 1280, .height = 720, .focus_valid = 0, .focus_position = 999, .autofocus = -1,
        .count = 2, .objects = {{.cls = 0, .name = "target\\\t\r\n", .conf = .75f, .x1 = 10, .y1 = 20, .x2 = 110, .y2 = 220},
            {.cls = 1, .name = "person", .conf = .9f, .x1 = 30, .y1 = 40, .x2 = 130, .y2 = 240}}};
    check_backpressure(&meta);
    char scratch[] = "/tmp/yolo-events-test-XXXXXX"; assert(mkdtemp(scratch));
    int original = open(".", O_RDONLY | O_DIRECTORY); assert(original >= 0 && chdir(scratch) == 0);
    assert(yolo_events_init(NULL) == 0 && yolo_events_init(NULL) == 0);
    assert(yolo_events_due(0, 1000));
    assert(!yolo_events_due(-1, 1000));
    assert(yolo_events_due(1, 1000) && yolo_events_due(1, 1000) && yolo_events_due(1, 1001));
    yolo_event_meta_t bad = meta; bad.objects[0].conf = NAN; assert(yolo_events_submit_meta(&bad) < 0);
    char first_day[9], ignored[REPORT_BUFFER_SIZE]; assert(format_report(&meta, first_day, ignored, sizeof(ignored)) > 0);
    submit_until_accepted(&meta); submit_until_accepted(&meta); /* Duplicate capture is not appended twice. */
    yolo_event_meta_t original_meta = meta;
    meta.seq = 8; meta.capture_unix_ms += 1000; meta.count = 1; meta.focus_valid = 1; meta.focus_position = 220; meta.autofocus = 1;
    submit_until_accepted(&meta);
    submit_until_accepted(&original_meta); /* Out-of-order duplicate is deduplicated too. */
    meta.seq = 81; meta.capture_unix_ms += 1000; meta.count = 0;
    submit_until_accepted(&meta); /* A frame without targets must still be present. */
    /* The timed flush must publish results while the writer remains running. */
    for (int i = 0; i < 600; ++i) {
        get_report(first_day);
        if (response_code == 200 && line_count(response) == 5) break;
        usleep(5000);
    }
    assert(response_code == 200 && line_count(response) == 5);
    assert(!strcmp(response_mime, "text/plain; charset=utf-8"));
    assert(!strncmp(response, report_header, sizeof(report_header) - 1));
    assert(strstr(response, ".123+0800\t1280\t720\ttarget\\\\\\t\\r\\n\t0.750000\t10.000\t20.000\t110.000\t220.000\t7\tnull\tnull\tnull\n"));
    assert(strstr(response, "\t8\t220\tnull\ttrue\n"));
    assert(strstr(response, "\tnone\tnull\tnull\tnull\tnull\tnull\t81\t220\tnull\ttrue\n"));
    assert(!strstr(response + sizeof(report_header) - 1, "capture_time"));
    size_t original_size = response_len;
    int retained_root = dup(events.root_fd); assert(retained_root >= 0);
    /* Simulate a short disk write then ENOSPC: old rows must remain intact. */
    write_budget = 5; assert(flush_report(first_day, "partial new row\n", 16) < 0); write_budget = -1;
    get_report(first_day); assert(response_code == 200 && response_len == original_size && line_count(response) == 5);

    /* A transient append failure retains the complete batch. A frame accepted
       during the retry also survives, and neither one is duplicated. */
    int previous_failures = atomic_load(&write_failures);
    write_budget = 5;
    meta.seq = 82; meta.count = 1; meta.capture_unix_ms += 1000;
    submit_until_accepted(&meta);
    for (int i = 0; i < 800 && atomic_load(&write_failures) < previous_failures + 2; ++i) usleep(5000);
    assert(atomic_load(&write_failures) >= previous_failures + 2);
    meta.seq = 83; meta.capture_unix_ms += 1000;
    submit_until_accepted(&meta);
    write_budget = -1;
    for (int i = 0; i < 800; ++i) {
        get_report(first_day);
        if (response_code == 200 && line_count(response) == 7) break;
        usleep(5000);
    }
    assert(response_code == 200 && line_count(response) == 7);
    assert(occurrence_count(response, "\t82\t220\tnull\ttrue\n") == 1);
    assert(occurrence_count(response, "\t83\t220\tnull\ttrue\n") == 1);
    assert(atomic_load(&recovery_logs) == 1);

    meta.seq = 9; meta.count = 0; meta.capture_unix_ms += 86400000ull;
    char second_day[9]; assert(format_report(&meta, second_day, ignored, sizeof(ignored)) > 0);
    assert(strcmp(first_day, second_day)); submit_until_accepted(&meta); memset(&meta, 0, sizeof(meta));
    yolo_events_destroy(); assert(!yolo_events_due(1, 9000)); yolo_events_destroy();
    get_report(second_day); assert(response_code == 200 && line_count(response) == 2 && strstr(response, "\t9\t220\tnull\ttrue\n"));

    /* Reopening the writer appends to a day's existing report without a header. */
    assert(yolo_events_init(NULL) == 0);
    meta = (yolo_event_meta_t){.seq = 10, .capture_unix_ms = 1793030401123ull + 86400000ull,
        .width = 1280, .height = 720, .autofocus = -1, .count = 1,
        .objects = {{.name = "new target", .conf = .5f, .x2 = 100, .y2 = 100}}};
    submit_until_accepted(&meta); yolo_events_destroy();
    get_report(second_day); assert(response_code == 200 && line_count(response) == 3);
    assert(!strstr(response + sizeof(report_header) - 1, "capture_time"));

    DIR *root = fdopendir(dup(retained_root)); assert(root);
    struct dirent *entry; int dates = 0, files_found = 0;
    while ((entry = readdir(root))) if (date_valid(entry->d_name)) {
        dates++; int fd = openat(retained_root, entry->d_name, O_RDONLY | O_DIRECTORY); assert(fd >= 0);
        DIR *files = fdopendir(fd); assert(files); struct dirent *file;
        while ((file = readdir(files))) if (strcmp(file->d_name, ".") && strcmp(file->d_name, "..")) { assert(strstr(file->d_name, ".rows")); files_found++; }
        closedir(files);
    }
    closedir(root); assert(dates == 2 && files_found == 2); /* No photos or JSON were generated. */
    http_detection_reports_list(NULL, 0, "GET", "/api/detections/reports", NULL);
    assert(response_code == 200 && strstr(response, first_day) && strstr(response, second_day));
    assert(strstr(response, second_day) < strstr(response, first_day));
    assert(mkdirat(retained_root, "20000101", 0755) == 0);
    int malicious = openat(retained_root, "20000101", O_RDONLY | O_DIRECTORY); assert(malicious >= 0);
    assert(symlinkat("/etc/passwd", malicious, "detections_20000101.xlsx") == 0); close(malicious);
    get_report("20000101"); assert(response_code == 404);
    http_detection_reports_list(NULL, 0, "GET", "/api/detections/reports", NULL);
    assert(!strstr(response, "20000101"));
    http_detection_file(NULL, 0, "GET", "/detectionfile/20261027/../secret.txt", NULL); assert(response_code == 404);
    errno = 0; assert(yolo_events_delete_report("20261027/../secret.txt") < 0 && errno == EINVAL);
    errno = 0; assert(yolo_events_delete_report("20000101/detections_20000101.xlsx") < 0 && errno == EINVAL);
    /* Deleting today's regular report is allowed; new records recreate it. */
    char relative[64]; snprintf(relative, sizeof(relative), "%s/detections_%s.xlsx", first_day, first_day);
    assert(yolo_events_init(NULL) == 0);
    assert(yolo_events_delete_report(relative) == 0);
    get_report(first_day); assert(response_code == 404);
    errno = 0; assert(yolo_events_delete_report(relative) < 0 && errno == ENOENT);
    meta = (yolo_event_meta_t){.seq = 12, .capture_unix_ms = 1793030402123ull,
        .width = 1280, .height = 720, .autofocus = 1, .count = 0};
    submit_until_accepted(&meta); yolo_events_destroy();
    get_report(first_day); assert(response_code == 200 && line_count(response) == 2 && strstr(response, "\tnone\tnull\tnull\tnull\tnull\tnull\t12\tnull\tnull\ttrue\n"));
    char request[160]; snprintf(request, sizeof(request), "POST /api/detections/reports/delete HTTP/1.1\r\n\r\n%s", relative);
    http_detection_report_delete(NULL, 0, "POST", "/api/detections/reports/delete", request);
    assert(response_code == 200 && !strcmp(response, "{\"deleted\":true}"));
    http_detection_report_delete(NULL, 0, "POST", "/api/detections/reports/delete", relative);
    assert(response_code == 404);
    http_detection_report_delete(NULL, 0, "POST", "/api/detections/reports/delete", "20261027/../secret.txt");
    assert(response_code == 400);
    http_detection_report_delete(NULL, 0, "POST", "/api/detections/reports/delete", "20000101/detections_20000101.xlsx");
    assert(response_code == 400);
    /* A full queue plus permanent storage failure must release a blocked
       inference producer before the caller joins that producer at shutdown. */
    assert(yolo_events_init(NULL) == 0);
    previous_failures = atomic_load(&write_failures);
    write_budget = 0; meta.seq = 13;
    submit_until_accepted(&meta);
    for (int i = 0; i < 800 && atomic_load(&write_failures) == previous_failures; ++i) usleep(5000);
    assert(atomic_load(&write_failures) > previous_failures);
    for (int i = 0; i < YOLO_EVENT_QUEUE_SIZE; ++i) {
        meta.seq++;
        assert(yolo_events_submit_meta(&meta) == 0);
    }
    producer_t blocked = {.lock = PTHREAD_MUTEX_INITIALIZER, .done_cond = PTHREAD_COND_INITIALIZER, .meta = meta};
    blocked.meta.seq++;
    pthread_t blocked_thread; assert(pthread_create(&blocked_thread, NULL, submit_one, &blocked) == 0);
    assert(!producer_done(&blocked, 20));
    unsigned long long stopping_at = monotonic_ms();
    yolo_events_prepare_stop();
    assert(producer_done(&blocked, 2000) && blocked.status == -1);
    pthread_join(blocked_thread, NULL);
    yolo_events_destroy();
    assert(monotonic_ms() - stopping_at < 3000u);
    assert(atomic_load(&shutdown_failure_logs) == 1);
    pthread_cond_destroy(&blocked.done_cond); pthread_mutex_destroy(&blocked.lock);
    write_budget = -1;
    clear_day(retained_root, first_day); clear_day(retained_root, second_day); clear_day(retained_root, "20000101");
    close(retained_root); assert(rmdir(YOLO_EVENTS_DIR) == 0 && rmdir("recordings") == 0);
    assert(fchdir(original) == 0); close(original); assert(rmdir(scratch) == 0);
    puts("PASS: every-frame TXT, transient storage recovery without omissions/duplicates, bounded failure shutdown, backpressure and protected report deletion/recreation");
    return 0;
}
