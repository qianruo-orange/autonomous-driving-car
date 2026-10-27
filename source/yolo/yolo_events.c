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

#define REPORT_BUFFER_SIZE 65536u
static const char report_header[] =
    "capture_time\twidth\theight\ttarget\tconfidence\tx1\ty1\tx2\ty2\tframe_seq\tfocus_position_raw\tfocal_length_mm\tautofocus\n";

static struct {
    pthread_mutex_t lock;
    pthread_cond_t ready, space;
    pthread_t thread;
    int started, stopping, stop_prepared, head, count, root_fd, recent_count, recent_next;
    struct { unsigned long long seq, capture_ms; } recent[YOLO_EVENT_QUEUE_SIZE];
    yolo_event_meta_t queue[YOLO_EVENT_QUEUE_SIZE];
    atomic_int enabled;
} events = {.lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER,
            .space = PTHREAD_COND_INITIALIZER, .root_fd = -1};

/* Report writes/deletes serialize independently of the producer queue. */
static pthread_mutex_t report_io_lock = PTHREAD_MUTEX_INITIALIZER;

static int digits(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}

/* Legacy downloads stay readable. This writer does not create images/JSON. */
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
    return p != seq && p - seq <= 20 && (!strcmp(p, ".jpg") || !strcmp(p, ".json"));
}

int yolo_report_rel_valid(const char *relative)
{
    return relative && strlen(relative) == 32 && digits(relative, 8) && relative[8] == '/' &&
        !strncmp(relative + 9, "detections_", 11) && !memcmp(relative, relative + 20, 8) &&
        !strcmp(relative + 28, ".txt");
}

static int append(char *out, size_t cap, size_t *length, const char *format, ...)
{
    if (*length >= cap) return -1;
    va_list ap; va_start(ap, format);
    int n = vsnprintf(out + *length, cap - *length, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *length) return -1;
    *length += (size_t)n;
    return 0;
}

static int append_target(char *out, size_t cap, size_t *length, const char *name)
{
    for (size_t i = 0; i < YOLO_EVENT_NAME_LEN && name[i]; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == '\\') { if (append(out, cap, length, "\\\\")) return -1; }
        else if (ch == '\t') { if (append(out, cap, length, "\\t")) return -1; }
        else if (ch == '\r') { if (append(out, cap, length, "\\r")) return -1; }
        else if (ch == '\n') { if (append(out, cap, length, "\\n")) return -1; }
        else if (ch < 32 || ch == 127) { if (append(out, cap, length, "\\x%02x", ch)) return -1; }
        else if (append(out, cap, length, "%c", ch)) return -1;
    }
    return 0;
}

static int format_report(const yolo_event_meta_t *meta, char day[9], char *out, size_t cap)
{
    time_t seconds = (time_t)(meta->capture_unix_ms / 1000u);
    struct tm date;
    char stamp[32], zone[16], focus[32] = "null";
    if (!localtime_r(&seconds, &date) || !strftime(day, 9, "%Y%m%d", &date) ||
        !strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &date) ||
        !strftime(zone, sizeof(zone), "%z", &date)) return -1;
    if (meta->focus_valid) snprintf(focus, sizeof(focus), "%d", meta->focus_position);
    const char *autofocus = meta->autofocus < 0 ? "null" : meta->autofocus ? "true" : "false";
    size_t length = 0;
    if (!meta->count) {
        if (append(out, cap, &length, "%s.%03llu%s\t%d\t%d\tnone\tnull\tnull\tnull\tnull\tnull\t%llu\t%s\tnull\t%s\n",
                   stamp, meta->capture_unix_ms % 1000u, zone, meta->width, meta->height,
                   meta->seq, focus, autofocus)) return -1;
    }
    for (int i = 0; i < meta->count; i++) {
        const yolo_event_object_t *object = &meta->objects[i];
        if (append(out, cap, &length, "%s.%03llu%s\t%d\t%d\t", stamp,
                   meta->capture_unix_ms % 1000u, zone, meta->width, meta->height) ||
            append_target(out, cap, &length, object->name) ||
            append(out, cap, &length, "\t%.6f\t%.3f\t%.3f\t%.3f\t%.3f\t%llu\t%s\tnull\t%s\n",
                   (double)object->conf, (double)object->x1, (double)object->y1,
                   (double)object->x2, (double)object->y2, meta->seq, focus, autofocus)) return -1;
    }
    return (int)length;
}

static int write_all(int fd, const void *bytes, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = write(fd, (const char *)bytes + offset, length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        offset += (size_t)n;
    }
    return 0;
}

/* Background-only append. A failed write is truncated back to its prior size,
 * preserving earlier complete rows. No video thread performs filesystem I/O. */
static int flush_report_locked(const char day[9], const char *rows, size_t length)
{
    if (!length) return 0;
    if (mkdirat(events.root_fd, day, 0755) && errno != EEXIST) return -1;
    int directory = openat(events.root_fd, day, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return -1;
    char name[32]; snprintf(name, sizeof(name), "detections_%s.txt", day);
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0644);
    close(directory);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); errno = EINVAL; return -1; }
    int rc = 0;
    if ((!st.st_size && write_all(fd, report_header, sizeof(report_header) - 1)) ||
        write_all(fd, rows, length) || fsync(fd)) rc = -1;
    if (rc) {
        int saved_errno = errno;
        if (ftruncate(fd, st.st_size) == 0) (void)fsync(fd);
        errno = saved_errno;
    }
    if (close(fd)) rc = -1;
    return rc;
}

static int flush_report(const char day[9], const char *rows, size_t length)
{
    pthread_mutex_lock(&report_io_lock);
    int result = flush_report_locked(day, rows, length);
    int saved_errno = errno;
    pthread_mutex_unlock(&report_io_lock);
    errno = saved_errno;
    return result;
}

int yolo_events_delete_report(const char *relative)
{
    if (!yolo_report_rel_valid(relative)) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&report_io_lock);
    int root = events.root_fd >= 0 ? dup(events.root_fd) :
        open(YOLO_EVENTS_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int directory = -1, result = -1;
    if (root < 0) goto done;
    char day[9]; memcpy(day, relative, 8); day[8] = '\0';
    directory = openat(root, day, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) goto done;
    struct stat st;
    if (fstatat(directory, relative + 9, &st, AT_SYMLINK_NOFOLLOW)) goto done;
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto done; }
    result = unlinkat(directory, relative + 9, 0);
done:;
    int saved_errno = errno;
    if (directory >= 0) close(directory);
    if (root >= 0) close(root);
    pthread_mutex_unlock(&report_io_lock);
    errno = saved_errno;
    return result;
}

static unsigned long long monotonic_ms(void)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000u + now.tv_nsec / 1000000u;
}

/* Keep an accepted batch until the storage recovers. With the writer waiting,
 * the bounded producer queue supplies backpressure instead of losing frames.
 * Shutdown has three attempts so an unavailable disk cannot hang service stop. */
static int flush_report_retry(const char day[9], const char *rows, size_t length)
{
    int failed = 0, stopping_attempts = 0;
    for (;;) {
        if (!flush_report(day, rows, length)) {
            if (failed) LOG_INFO("yolo events: daily report storage recovered; pending batch saved");
            return 0;
        }
        if (!failed++) LOG_ERROR("yolo events: daily report append failed; retaining batch for retry: %s", strerror(errno));
        pthread_mutex_lock(&events.lock);
        int stopping = events.stopping || events.stop_prepared;
        int queued = events.count;
        if (stopping && ++stopping_attempts >= 3) {
            atomic_store(&events.enabled, 0);
            pthread_cond_broadcast(&events.space);
            pthread_mutex_unlock(&events.lock);
            LOG_ERROR("yolo events: shutdown storage failure; %zu buffered report bytes and %d queued frames remain unsaved", length, queued);
            return -1;
        }
        pthread_mutex_unlock(&events.lock);
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 200000000L};
        while (nanosleep(&pause, &pause) && errno == EINTR) {}
    }
}

static void *writer(void *unused)
{
    (void)unused;
    char batch[REPORT_BUFFER_SIZE], rows[REPORT_BUFFER_SIZE], day[9] = "";
    size_t used = 0;
    unsigned long long deadline = 0;
    for (;;) {
        yolo_event_meta_t item;
        int have_item = 0, stopping;
        pthread_mutex_lock(&events.lock);
        while (!events.count && !events.stopping) {
            if (!used) pthread_cond_wait(&events.ready, &events.lock);
            else {
                unsigned long long now = monotonic_ms();
                if (now >= deadline) break;
                struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
                until.tv_nsec += (long)(deadline - now) * 1000000L;
                until.tv_sec += until.tv_nsec / 1000000000L;
                until.tv_nsec %= 1000000000L;
                pthread_cond_timedwait(&events.ready, &events.lock, &until);
            }
        }
        stopping = events.stopping;
        if (events.count) {
            item = events.queue[events.head];
            events.head = (events.head + 1) % YOLO_EVENT_QUEUE_SIZE;
            events.count--; have_item = 1;
            pthread_cond_signal(&events.space);
        }
        pthread_mutex_unlock(&events.lock);
        if (used && monotonic_ms() >= deadline) {
            if (flush_report_retry(day, batch, used)) return NULL;
            used = 0;
        }
        if (have_item) {
            char item_day[9];
            int n = format_report(&item, item_day, rows, sizeof(rows));
            if (n < 0) { LOG_ERROR("yolo events: invalid daily report metadata"); continue; }
            if (used && (strcmp(day, item_day) || used + (size_t)n > sizeof(batch))) {
                if (flush_report_retry(day, batch, used)) return NULL;
                used = 0;
            }
            if (!used) { memcpy(day, item_day, sizeof(day)); deadline = monotonic_ms() + 1000u; }
            memcpy(batch + used, rows, (size_t)n); used += (size_t)n;
        } else if (stopping) break;
    }
    if (used) (void)flush_report_retry(day, batch, used);
    return NULL;
}

static int create_root(const char *root)
{
    if (!root || !*root || strlen(root) >= PATH_MAX) return -1;
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s", root);
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
    events.stopping = events.stop_prepared = events.head = events.count = events.recent_count = events.recent_next = 0;
    int rc = pthread_create(&events.thread, NULL, writer, NULL);
    if (rc) {
        close(events.root_fd); events.root_fd = -1;
        pthread_mutex_unlock(&events.lock); errno = rc; return -1;
    }
    events.started = 1; atomic_store(&events.enabled, 1);
    pthread_mutex_unlock(&events.lock);
    return 0;
}

int yolo_events_due(int count, unsigned long long now_ms)
{
    (void)now_ms;
    return count >= 0 && atomic_load(&events.enabled);
}

int yolo_events_submit_meta(const yolo_event_meta_t *meta)
{
    if (!meta || meta->count < 0 || meta->count > YOLO_MAX_DETS || meta->width <= 0 || meta->height <= 0 ||
        meta->autofocus < -1 || meta->autofocus > 1 || !atomic_load(&events.enabled)) return -1;
    for (int i = 0; i < meta->count; i++) {
        const yolo_event_object_t *object = &meta->objects[i];
        if (!isfinite(object->conf) || !isfinite(object->x1) || !isfinite(object->y1) ||
            !isfinite(object->x2) || !isfinite(object->y2)) return -1;
    }
    pthread_mutex_lock(&events.lock);
    for (;;) {
        if (!atomic_load(&events.enabled) || events.stopping) { pthread_mutex_unlock(&events.lock); return -1; }
        for (int i = 0; i < events.recent_count; ++i)
            if (events.recent[i].seq == meta->seq && events.recent[i].capture_ms == meta->capture_unix_ms) {
                pthread_mutex_unlock(&events.lock); return 0;
            }
        if (events.count < YOLO_EVENT_QUEUE_SIZE) break;
        pthread_cond_wait(&events.space, &events.lock);
    }
    int index = (events.head + events.count) % YOLO_EVENT_QUEUE_SIZE;
    events.queue[index] = *meta;
    for (int i = 0; i < meta->count; i++) events.queue[index].objects[i].name[YOLO_EVENT_NAME_LEN - 1] = '\0';
    events.recent[events.recent_next].seq = meta->seq;
    events.recent[events.recent_next].capture_ms = meta->capture_unix_ms;
    events.recent_next = (events.recent_next + 1) % YOLO_EVENT_QUEUE_SIZE;
    if (events.recent_count < YOLO_EVENT_QUEUE_SIZE) events.recent_count++;
    events.count++; pthread_cond_signal(&events.ready);
    pthread_mutex_unlock(&events.lock);
    return 0;
}

/* Source compatibility only: no image bytes are copied or saved. */
int yolo_events_submit(const unsigned char *jpeg, size_t len, const yolo_event_meta_t *meta)
{
    (void)jpeg; (void)len;
    return yolo_events_submit_meta(meta);
}

void yolo_events_prepare_stop(void)
{
    pthread_mutex_lock(&events.lock);
    events.stop_prepared = 1;
    pthread_mutex_unlock(&events.lock);
}

void yolo_events_destroy(void)
{
    atomic_store(&events.enabled, 0);
    pthread_mutex_lock(&events.lock);
    events.stopping = 1;
    pthread_cond_broadcast(&events.ready);
    pthread_cond_broadcast(&events.space);
    if (!events.started) { pthread_mutex_unlock(&events.lock); return; }
    pthread_mutex_unlock(&events.lock);
    pthread_join(events.thread, NULL);
    pthread_mutex_lock(&events.lock);
    pthread_mutex_lock(&report_io_lock);
    close(events.root_fd); events.root_fd = -1; events.started = 0;
    pthread_mutex_unlock(&report_io_lock);
    pthread_mutex_unlock(&events.lock);
}
