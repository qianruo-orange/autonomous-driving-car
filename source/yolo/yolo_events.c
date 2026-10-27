/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "yolo/yolo_events.h"
#include "core/log.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct {
    unsigned char *jpeg;
    size_t len;
    yolo_event_meta_t meta;
} event_item_t;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    pthread_t thread;
    int started, stopping, head, count;
    int root_fd;
    event_item_t queue[YOLO_EVENT_QUEUE_SIZE];
    atomic_int enabled;
    atomic_ullong next_due_ms;
} events = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .ready = PTHREAD_COND_INITIALIZER,
    .root_fd = -1
};

static int digits(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}

int yolo_event_rel_valid(const char *relative)
{
    if (!relative) return 0;
    size_t n = strlen(relative);
    if (n < 29 || n > 50 || !digits(relative, 8) || relative[8] != '/') return 0;
    const char *p = relative + 9;
    if (strncmp(p, "det_", 4) || !digits(p + 4, 6) || p[10] != '_' ||
        !digits(p + 11, 3) || p[14] != '_') return 0;
    p += 15;
    const char *seq = p;
    while (*p >= '0' && *p <= '9') p++;
    if (p == seq || p - seq > 20) return 0;
    return strcmp(p, ".jpg") == 0 || strcmp(p, ".json") == 0;
}

static int append(char *out, size_t cap, size_t *length, const char *format, ...)
{
    if (*length >= cap) return -1;
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(out + *length, cap - *length, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *length) return -1;
    *length += (size_t)n;
    return 0;
}

static int append_name(char *out, size_t cap, size_t *length, const char *name)
{
    if (append(out, cap, length, "\"")) return -1;
    for (size_t i = 0; i < YOLO_EVENT_NAME_LEN && name[i]; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == '"' || ch == '\\') {
            if (append(out, cap, length, "\\%c", ch)) return -1;
        } else if (ch < 32) {
            if (append(out, cap, length, "\\u%04x", ch)) return -1;
        } else if (append(out, cap, length, "%c", ch)) return -1;
    }
    return append(out, cap, length, "\"");
}

static int format_metadata(const yolo_event_meta_t *m, char *out, size_t cap)
{
    size_t n = 0;
    char focus[32] = "null", autofocus[8] = "null", sample[32] = "null", age[32] = "null";
    if (m->focus_valid) snprintf(focus, sizeof(focus), "%d", m->focus_position);
    if (m->autofocus == 0 || m->autofocus == 1)
        snprintf(autofocus, sizeof(autofocus), "%s", m->autofocus ? "true" : "false");
    if (m->focus_sample_monotonic_ms) {
        snprintf(sample, sizeof(sample), "%llu", m->focus_sample_monotonic_ms);
        if (m->capture_monotonic_ms >= m->focus_sample_monotonic_ms)
            snprintf(age, sizeof(age), "%llu", m->capture_monotonic_ms - m->focus_sample_monotonic_ms);
    }
    if (append(out, cap, &n,
        "{\"schema_version\":1,\"seq\":%llu,\"capture_monotonic_ms\":%llu,"
        "\"capture_unix_ms\":%llu,\"capture_unix_estimated\":true,\"width\":%d,\"height\":%d,\"object_count\":%d,"
        "\"focus_position_raw\":%s,\"focus_unit\":\"device_units\","
        "\"focus_sample_monotonic_ms\":%s,\"focus_sample_age_ms\":%s,"
        "\"autofocus\":%s,\"focal_length_mm\":null,\"objects\":[",
        m->seq, m->capture_monotonic_ms, m->capture_unix_ms, m->width, m->height,
        m->count, focus, sample, age, autofocus)) return -1;
    for (int i = 0; i < m->count; i++) {
        const yolo_event_object_t *obj = &m->objects[i];
        if (append(out, cap, &n, "%s{\"cls\":%d,\"name\":", i ? "," : "", obj->cls) ||
            append_name(out, cap, &n, obj->name) ||
            append(out, cap, &n, ",\"conf\":%.6f,\"bbox\":[%.3f,%.3f,%.3f,%.3f]}",
                (double)obj->conf, (double)obj->x1, (double)obj->y1,
                (double)obj->x2, (double)obj->y2)) return -1;
    }
    if (append(out, cap, &n, "]}\n")) return -1;
    return (int)n;
}

static int write_file(int dir, const char *name, const void *data, size_t size)
{
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) return -1;
    const unsigned char *bytes = data;
    size_t off = 0;
    int rc = 0;
    while (off < size) {
        ssize_t written = write(fd, bytes + off, size - off);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { rc = -1; break; }
        off += (size_t)written;
    }
    if (rc == 0 && fsync(fd)) rc = -1;
    if (close(fd)) rc = -1;
    if (rc) unlinkat(dir, name, 0);
    return rc;
}

static int save_event(const event_item_t *item)
{
    char metadata[32768];
    int metadata_len = format_metadata(&item->meta, metadata, sizeof(metadata));
    if (metadata_len < 0) return -1;
    time_t seconds = (time_t)(item->meta.capture_unix_ms / 1000u);
    struct tm date;
    if (!localtime_r(&seconds, &date)) return -1;
    char day[16], stamp[16], base[80], jpg[96], json[96], jpg_tmp[96], json_tmp[96];
    if (!strftime(day, sizeof(day), "%Y%m%d", &date) ||
        !strftime(stamp, sizeof(stamp), "%H%M%S", &date)) return -1;
    if (mkdirat(events.root_fd, day, 0755) && errno != EEXIST) return -1;
    int dir = openat(events.root_fd, day, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return -1;
    snprintf(base, sizeof(base), "det_%s_%03llu_%llu", stamp,
             item->meta.capture_unix_ms % 1000u, item->meta.seq);
    snprintf(jpg, sizeof(jpg), "%s.jpg", base);
    snprintf(json, sizeof(json), "%s.json", base);
    snprintf(jpg_tmp, sizeof(jpg_tmp), ".%s.jpg.tmp", base);
    snprintf(json_tmp, sizeof(json_tmp), ".%s.json.tmp", base);
    struct stat existing;
    int rc = -1, jpg_published = 0;
    /* Never replace a completed pair, including after a process restart. */
    if (fstatat(dir, jpg, &existing, AT_SYMLINK_NOFOLLOW) == 0 ||
        fstatat(dir, json, &existing, AT_SYMLINK_NOFOLLOW) == 0) goto done;
    if (write_file(dir, jpg_tmp, item->jpeg, item->len) ||
        write_file(dir, json_tmp, metadata, (size_t)metadata_len)) goto done;
    if (renameat(dir, jpg_tmp, dir, jpg)) goto done;
    jpg_published = 1;
    /* Publish JSON last: readers use it as the completed-pair marker. */
    if (renameat(dir, json_tmp, dir, json)) goto done;
    (void)fsync(dir);
    rc = 0;
done:
    unlinkat(dir, jpg_tmp, 0);
    unlinkat(dir, json_tmp, 0);
    if (rc && jpg_published) unlinkat(dir, jpg, 0);
    close(dir);
    return rc;
}

static void *writer(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&events.lock);
    for (;;) {
        while (!events.count && !events.stopping) pthread_cond_wait(&events.ready, &events.lock);
        if (!events.count && events.stopping) break;
        event_item_t item = events.queue[events.head];
        memset(&events.queue[events.head], 0, sizeof(item));
        events.head = (events.head + 1) % YOLO_EVENT_QUEUE_SIZE;
        events.count--;
        pthread_mutex_unlock(&events.lock);
        if (save_event(&item)) LOG_ERROR("yolo events: snapshot save failed: %s", strerror(errno));
        free(item.jpeg);
        pthread_mutex_lock(&events.lock);
    }
    pthread_mutex_unlock(&events.lock);
    return NULL;
}

static int create_root(const char *root)
{
    if (!root || !*root || strlen(root) >= PATH_MAX) return -1;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", root);
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(path, 0755) && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(path, 0755) && errno != EEXIST) return -1;
    return open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

int yolo_events_init(const char *root)
{
    pthread_mutex_lock(&events.lock);
    if (events.started) { pthread_mutex_unlock(&events.lock); return 0; }
    events.root_fd = create_root(root ? root : YOLO_EVENTS_DIR);
    if (events.root_fd < 0) { pthread_mutex_unlock(&events.lock); return -1; }
    events.stopping = events.head = events.count = 0;
    atomic_store(&events.next_due_ms, 0);
    int rc = pthread_create(&events.thread, NULL, writer, NULL);
    if (rc) {
        close(events.root_fd); events.root_fd = -1;
        pthread_mutex_unlock(&events.lock); errno = rc; return -1;
    }
    events.started = 1;
    atomic_store(&events.enabled, 1);
    pthread_mutex_unlock(&events.lock);
    return 0;
}

int yolo_events_due(int count, unsigned long long monotonic_ms)
{
    if (count <= 0 || !atomic_load(&events.enabled)) return 0;
    unsigned long long expected = atomic_load(&events.next_due_ms);
    if (monotonic_ms < expected || monotonic_ms > ULLONG_MAX - 1000u) return 0;
    return atomic_compare_exchange_strong(&events.next_due_ms, &expected, monotonic_ms + 1000u);
}

int yolo_events_submit(const unsigned char *jpeg, size_t len, const yolo_event_meta_t *meta)
{
    if (!jpeg || !len || len > YOLO_EVENT_JPEG_MAX || !meta ||
        meta->count < 1 || meta->count > YOLO_MAX_DETS || meta->width <= 0 || meta->height <= 0 ||
        meta->autofocus < -1 || meta->autofocus > 1 || !atomic_load(&events.enabled)) return -1;
    for (int i = 0; i < meta->count; i++) {
        const yolo_event_object_t *obj = &meta->objects[i];
        if (!isfinite(obj->conf) || !isfinite(obj->x1) || !isfinite(obj->y1) ||
            !isfinite(obj->x2) || !isfinite(obj->y2)) return -1;
    }
    unsigned char *copy = malloc(len);
    if (!copy) return -1;
    memcpy(copy, jpeg, len);
    if (pthread_mutex_trylock(&events.lock)) { free(copy); return -1; }
    if (!atomic_load(&events.enabled) || events.stopping || events.count == YOLO_EVENT_QUEUE_SIZE) {
        pthread_mutex_unlock(&events.lock); free(copy); return -1;
    }
    int index = (events.head + events.count) % YOLO_EVENT_QUEUE_SIZE;
    events.queue[index].jpeg = copy;
    events.queue[index].len = len;
    events.queue[index].meta = *meta;
    for (int i = 0; i < meta->count; i++)
        events.queue[index].meta.objects[i].name[YOLO_EVENT_NAME_LEN - 1] = '\0';
    events.count++;
    pthread_cond_signal(&events.ready);
    pthread_mutex_unlock(&events.lock);
    return 0;
}

void yolo_events_destroy(void)
{
    atomic_store(&events.enabled, 0);
    pthread_mutex_lock(&events.lock);
    if (!events.started) { pthread_mutex_unlock(&events.lock); return; }
    events.stopping = 1;
    pthread_cond_broadcast(&events.ready);
    pthread_mutex_unlock(&events.lock);
    pthread_join(events.thread, NULL);
    pthread_mutex_lock(&events.lock);
    close(events.root_fd); events.root_fd = -1; events.started = 0;
    pthread_mutex_unlock(&events.lock);
}
