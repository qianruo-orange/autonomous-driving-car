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
#include <dirent.h>
#include <spawn.h>
#include <sys/wait.h>

#define REPORT_BUFFER_SIZE 65536u

static struct {
    pthread_mutex_t lock;
    pthread_cond_t ready, space;
    pthread_t thread;
    int started, stopping, stop_prepared, head, count, root_fd, recent_count, recent_next;
    struct { unsigned long long seq, capture_ms; } recent[YOLO_EVENT_QUEUE_SIZE];
    yolo_event_meta_t queue[YOLO_EVENT_QUEUE_SIZE];
    unsigned long long queue_epoch[YOLO_EVENT_QUEUE_SIZE];
    atomic_int enabled;
} events = {.lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER,
            .space = PTHREAD_COND_INITIALIZER, .root_fd = -1};

/* Report writes/deletes serialize independently of the producer queue. */
static pthread_mutex_t report_io_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_ullong report_epoch = 1;
static unsigned long long reports_cleared_epoch = 1;
static struct report_invalidation {
    char day[9];
    unsigned long long epoch;
    struct report_invalidation *next;
} *report_invalidations;

static int report_epoch_valid_locked(const char day[9], unsigned long long epoch)
{
    if (epoch < reports_cleared_epoch) return 0;
    for (struct report_invalidation *p = report_invalidations; p; p = p->next)
        if (!strcmp(day, p->day) && epoch < p->epoch) return 0;
    return 1;
}

static int invalidate_report_locked(const char day[9])
{
    struct report_invalidation *entry = report_invalidations;
    while (entry && strcmp(entry->day, day)) entry = entry->next;
    if (!entry) {
        entry = calloc(1, sizeof(*entry));
        if (!entry) return -1;
        memcpy(entry->day, day, sizeof(entry->day));
        entry->next = report_invalidations; report_invalidations = entry;
    }
    entry->epoch = atomic_fetch_add(&report_epoch, 1) + 1;
    return 0;
}

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
    if (!relative || strlen(relative) < 28 || !digits(relative, 8) || relative[8] != '/') return 0;
    return (strlen(relative) == 28 && !strncmp(relative + 9, "result_", 7) &&
        !memcmp(relative, relative + 16, 8) && !strcmp(relative + 24, ".txt")) ||
        (strlen(relative) == 32 && !strncmp(relative + 9, "detections_", 11) &&
        !memcmp(relative, relative + 20, 8) && !strcmp(relative + 28, ".txt"));
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

static int append_escaped(char *out, size_t cap, size_t *length, const char *name, size_t limit)
{
    for (size_t i = 0; i < limit && name[i]; i++) {
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

static int append_target(char *out, size_t cap, size_t *length, const char *name)
{
    return append_escaped(out, cap, length, name, YOLO_EVENT_NAME_LEN);
}

/* The public TXT follows the frame/object layout used by field tools. Focus
 * positions have no calibrated millimetre mapping, so label raw values honestly. */
static int format_report(const yolo_event_meta_t *meta, char day[9], char *out, size_t cap)
{
    time_t seconds = (time_t)(meta->capture_unix_ms / 1000u);
    struct tm date;
    char stamp[32], zone[16], focus[32] = "unknown";
    if (!localtime_r(&seconds, &date) || !strftime(day, 9, "%Y%m%d", &date) ||
        !strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &date) ||
        !strftime(zone, sizeof(zone), "%z", &date)) return -1;
    if (meta->focus_valid) snprintf(focus, sizeof(focus), "raw:%d", meta->focus_position);
    size_t length = 0;
    if (append(out, cap, &length, "%s.%03llu%s：num[%d] size[%d %d] FocalLength[%s]\n",
               stamp, meta->capture_unix_ms % 1000u, zone, meta->count,
               meta->width, meta->height, focus)) return -1;
    for (int i = 0; i < meta->count; i++) {
        const yolo_event_object_t *object = &meta->objects[i];
        if (append(out, cap, &length, "\t") ||
            append_target(out, cap, &length, object->name) ||
            append(out, cap, &length, " %.6f position[%.3f %.3f %.3f %.3f]\n",
                   (double)object->conf, (double)object->x1, (double)object->x2,
                   (double)object->y1, (double)object->y2)) return -1;
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
    char name[64]; snprintf(name, sizeof(name), "result_%s.txt", day);
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0644);
    close(directory);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); errno = EINVAL; return -1; }
    int rc = 0;
    if (write_all(fd, rows, length) || fsync(fd)) rc = -1;
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

static int flush_report_epoch(const char day[9], const char *rows, size_t length,
                              unsigned long long epoch)
{
    pthread_mutex_lock(&report_io_lock);
    int result = report_epoch_valid_locked(day, epoch) ? flush_report_locked(day, rows, length) : 1;
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
    char rows[64], workbook[64], current[64], legacy[64];
    snprintf(rows, sizeof(rows), ".detections_%s.rows", day);
    snprintf(workbook, sizeof(workbook), "detections_%s.xlsx", day);
    snprintf(current, sizeof(current), "result_%s.txt", day);
    snprintf(legacy, sizeof(legacy), "detections_%s.txt", day);
    const char *names[] = {current, legacy, rows, workbook};
    int found[4] = {0};
    for (size_t i = 0; i < 4; i++) {
        struct stat st;
        if (!fstatat(directory, names[i], &st, AT_SYMLINK_NOFOLLOW)) {
            if (!S_ISREG(st.st_mode)) { errno = EINVAL; goto done; }
            found[i] = 1;
        } else if (errno != ENOENT) goto done;
    }
    if (!found[0] && !found[1] && !found[2] && !found[3]) { errno = ENOENT; goto done; }
    if (invalidate_report_locked(day)) goto done;
    result = 0;
    for (size_t i = 0; i < 4; i++)
        if (found[i] && unlinkat(directory, names[i], 0)) { result = -1; break; }
done:;
    int saved_errno = errno;
    if (directory >= 0) close(directory);
    if (root >= 0) close(root);
    pthread_mutex_unlock(&report_io_lock);
    errno = saved_errno;
    return result;
}

/* Open a fresh directory description: readdir must never advance root_fd. */
static int report_root_open(void)
{
    return events.root_fd >= 0 ? openat(events.root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC) :
        open(YOLO_EVENTS_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

static void remove_report_snapshot(const char *path)
{
    DIR *root = opendir(path);
    if (!root) return;
    struct dirent *entry;
    while ((entry = readdir(root))) {
        if (strlen(entry->d_name) != 8 || !digits(entry->d_name, 8)) continue;
        int day = openat(dirfd(root), entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (day < 0) continue;
        char name[64]; snprintf(name, sizeof(name), "result_%.8s.txt", entry->d_name);
        (void)unlinkat(day, name, 0); close(day);
        (void)unlinkat(dirfd(root), entry->d_name, AT_REMOVEDIR);
    }
    closedir(root); (void)rmdir(path);
}

/* Caller enters/returns holding report_io_lock. Pin a complete flushed TXT
 * prefix, then copy outside the lock. Appends cannot alter that prefix; a clear
 * unlinks the inode. Reject a snapshot if deletion changed its generation. */
static int snapshot_reports(const char *snapshot, unsigned *count, const char *selected,
                            unsigned long long *epoch)
{
    *epoch = atomic_load(&report_epoch);
    int root_fd = report_root_open();
    if (root_fd < 0) return -1;
    DIR *root = fdopendir(root_fd);
    if (!root) { close(root_fd); return -1; }
    int stage = open(snapshot, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (stage < 0) { closedir(root); return -1; }
    unsigned long long total = 0;
    int result = 0;
    for (;;) {
        errno = 0; struct dirent *entry = readdir(root);
        if (!entry) { if (errno) result = -1; break; }
        if (strlen(entry->d_name) != 8 || !digits(entry->d_name, 8)) continue;
        if (selected && strncmp(selected, entry->d_name, 8)) continue;
        int day = openat(root_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (day < 0) continue;
        char name[64]; snprintf(name, sizeof(name), "result_%.8s.txt", entry->d_name);
        const char *source_name = name;
        int source = openat(day, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        close(day);
        if (source < 0) continue;
        struct stat st;
        if (fstat(source, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0) { close(source); continue; }
        if ((unsigned long long)st.st_size > 2ull * 1024u * 1024u * 1024u - total) {
            close(source); errno = EFBIG; result = -1; break;
        }
        if (mkdirat(stage, entry->d_name, 0700)) { close(source); result = -1; break; }
        int target_day = openat(stage, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int target = target_day < 0 ? -1 : openat(target_day, source_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (target_day >= 0) close(target_day);
        if (target < 0) { close(source); result = -1; break; }
        pthread_mutex_unlock(&report_io_lock);
        off_t remaining = st.st_size;
        char bytes[65536];
        while (remaining) {
            size_t length = remaining > (off_t)sizeof(bytes) ? sizeof(bytes) : (size_t)remaining;
            ssize_t n = read(source, bytes, length);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { if (!n) errno = EIO; result = -1; break; }
            if (write_all(target, bytes, (size_t)n)) { result = -1; break; }
            remaining -= n;
        }
        if (!result) {
            struct timespec times[] = {st.st_atim, st.st_mtim};
            if (futimens(target, times)) result = -1;
        }
        int saved_errno = errno;
        close(source); close(target); errno = saved_errno;
        pthread_mutex_lock(&report_io_lock);
        if (!result && *epoch != atomic_load(&report_epoch)) { result = -1; errno = EAGAIN; }
        if (result) break;
        total += (unsigned long long)st.st_size; (*count)++;
    }
    int saved_errno = errno;
    close(stage); closedir(root); errno = saved_errno;
    return result;
}

static int run_report_command(char *const command[], int output)
{
    char *arguments[24] = {"nice", "-n", "10"};
    size_t n = 3;
    for (size_t i = 0; command[i]; i++) {
        if (n + 1 >= sizeof(arguments) / sizeof(arguments[0])) {
            LOG_ERROR("yolo report command arguments exceed capacity"); errno = E2BIG; return -1;
        }
        arguments[n++] = command[i];
    }
    arguments[n] = NULL;
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    const char *phase = "file actions";
    if (!result) {
        result = output >= 0 ? posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO) :
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        if (!result) result = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        pid_t child;
        extern char **environ;
        if (!result) {
            phase = "posix_spawnp";
            result = posix_spawnp(&child, "nice", &actions, NULL, arguments, environ);
        }
        posix_spawn_file_actions_destroy(&actions);
        if (result) LOG_ERROR("yolo report command %s %s failed: %s (error %d)",
                              command[0], phase, strerror(result), result);
        if (!result) {
            int status; pid_t waited;
            do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
            if (waited < 0) {
                LOG_ERROR("yolo report command %s waitpid failed: %s", command[0], strerror(errno));
                result = EIO;
            } else if (WIFSIGNALED(status)) {
                LOG_ERROR("yolo report command %s terminated by signal %d", command[0], WTERMSIG(status));
                result = EIO;
            } else if (!WIFEXITED(status) || WEXITSTATUS(status)) {
                LOG_ERROR("yolo report command %s exited with status %d", command[0],
                          WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                result = EIO;
            }
        }
    } else LOG_ERROR("yolo report command %s file actions failed: %s (error %d)", command[0], strerror(result), result);
    if (result) { errno = result; return -1; }
    return 0;
}

static int snapshot_epoch_unchanged(unsigned long long epoch)
{
    pthread_mutex_lock(&report_io_lock);
    int unchanged = epoch == atomic_load(&report_epoch);
    pthread_mutex_unlock(&report_io_lock);
    if (!unchanged) errno = EAGAIN;
    return unchanged;
}

static int report_failure(const char *operation, const char *phase, int error)
{
    if (error == ENOENT) LOG_INFO("yolo report %s %s: no report available", operation, phase);
    else LOG_ERROR("yolo report %s %s failed: %s (error %d)", operation, phase, strerror(error), error);
    errno = error;
    return -1;
}

int yolo_events_export_report(const char *relative)
{
    if (!yolo_report_rel_valid(relative)) { errno = EINVAL; return -1; }
    char snapshot[] = "/tmp/gateway-detections-snapshot-XXXXXX";
    if (!mkdtemp(snapshot)) return report_failure("TXT", "temporary directory", errno);
    unsigned count = 0; unsigned long long epoch;
    pthread_mutex_lock(&report_io_lock);
    int result = snapshot_reports(snapshot, &count, relative, &epoch);
    int saved_errno = errno;
    pthread_mutex_unlock(&report_io_lock);
    const char *phase = "snapshot";
    if (!result && !count) { result = -1; saved_errno = ENOENT; }
    int output = -1;
    if (!result) {
        phase = "snapshot open";
        char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%.8s/result_%.8s.txt", snapshot, relative, relative);
        output = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (output < 0) saved_errno = errno;
    }
    remove_report_snapshot(snapshot);
    if (output < 0) return report_failure("TXT", phase, saved_errno);
    errno = saved_errno;
    return output;
}

int yolo_events_pack_reports(unsigned *file_count)
{
    unsigned count = 0;
    unsigned long long epoch;
    char snapshot[] = "/tmp/gateway-detections-snapshot-XXXXXX";
    if (!mkdtemp(snapshot)) return report_failure("archive", "temporary directory", errno);
    pthread_mutex_lock(&report_io_lock);
    int result = snapshot_reports(snapshot, &count, NULL, &epoch);
    int saved_errno = errno;
    pthread_mutex_unlock(&report_io_lock);
    if (result || !count) {
        remove_report_snapshot(snapshot); return report_failure("archive", "snapshot", result ? saved_errno : ENOENT);
    }
    char output_path[] = "/tmp/gateway-detections-archive-XXXXXX";
    int output = mkstemp(output_path);
    if (output < 0) { saved_errno = errno; remove_report_snapshot(snapshot); return report_failure("archive", "temporary file", saved_errno); }
    unlink(output_path);
    char *arguments[] = {"tar", "-I", "gzip -1", "-cf", "-", "-C", snapshot, ".", NULL};
    result = run_report_command(arguments, output);
    const char *phase = "compression";
    if (!result && !snapshot_epoch_unchanged(epoch)) { phase = "generation check"; result = -1; }
    saved_errno = errno;
    remove_report_snapshot(snapshot);
    if (result || lseek(output, 0, SEEK_SET) < 0) {
        if (!result) { phase = "rewind"; saved_errno = errno; }
        close(output); return report_failure("archive", phase, saved_errno);
    }
    if (file_count) *file_count = count;
    return output;
}

int yolo_events_clear_reports(unsigned *deleted)
{
    unsigned count = 0;
    pthread_mutex_lock(&report_io_lock);
    reports_cleared_epoch = atomic_fetch_add(&report_epoch, 1) + 1;
    while (report_invalidations) {
        struct report_invalidation *old = report_invalidations;
        report_invalidations = old->next; free(old);
    }
    int root_fd = report_root_open();
    if (root_fd < 0) {
        int saved_errno = errno; pthread_mutex_unlock(&report_io_lock);
        if (saved_errno == ENOENT) { if (deleted) *deleted = 0; return 0; }
        errno = saved_errno; return -1;
    }
    DIR *root = fdopendir(root_fd);
    if (!root) { int saved_errno = errno; close(root_fd); pthread_mutex_unlock(&report_io_lock); errno = saved_errno; return -1; }
    int result = 0;
    for (;;) {
        errno = 0; struct dirent *entry = readdir(root);
        if (!entry) { if (errno) result = -1; break; }
        if (strlen(entry->d_name) != 8 || !digits(entry->d_name, 8)) continue;
        int day = openat(root_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (day < 0) continue;
        char name[64], rows[64], workbook[64], legacy[64];
        snprintf(name, sizeof(name), "result_%.8s.txt", entry->d_name);
        snprintf(rows, sizeof(rows), ".detections_%.8s.rows", entry->d_name);
        snprintf(workbook, sizeof(workbook), "detections_%.8s.xlsx", entry->d_name);
        snprintf(legacy, sizeof(legacy), "detections_%.8s.txt", entry->d_name);
        const char *names[] = {name, rows, workbook, legacy};
        int found = 0;
        for (size_t i = 0; i < 4; i++) {
            struct stat st;
            if (!fstatat(day, names[i], &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode)) {
                if (unlinkat(day, names[i], 0)) { result = -1; break; }
                found = 1;
            }
        }
        close(day);
        if (result) break;
        if (found) count++;
    }
    int saved_errno = errno;
    closedir(root); pthread_mutex_unlock(&report_io_lock);
    if (deleted) *deleted = count;
    errno = saved_errno; return result;
}

static unsigned long long monotonic_ms(void)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000u + now.tv_nsec / 1000000u;
}

/* Keep an accepted batch until the storage recovers. With the writer waiting,
 * the bounded producer queue supplies backpressure instead of losing frames.
 * Shutdown has three attempts so an unavailable disk cannot hang service stop. */
static int flush_report_retry(const char day[9], const char *rows, size_t length,
                              unsigned long long epoch)
{
    int failed = 0, stopping_attempts = 0;
    for (;;) {
        int flushed = flush_report_epoch(day, rows, length, epoch);
        if (flushed >= 0) {
            if (failed && !flushed) LOG_INFO("yolo events: daily report storage recovered; pending batch saved");
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
    unsigned long long batch_epoch = 0;
    unsigned long long deadline = 0;
    for (;;) {
        yolo_event_meta_t item;
        unsigned long long item_epoch = 0;
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
            item_epoch = events.queue_epoch[events.head];
            events.head = (events.head + 1) % YOLO_EVENT_QUEUE_SIZE;
            events.count--; have_item = 1;
            pthread_cond_signal(&events.space);
        }
        pthread_mutex_unlock(&events.lock);
        if (used && monotonic_ms() >= deadline) {
            if (flush_report_retry(day, batch, used, batch_epoch)) return NULL;
            used = 0;
        }
        if (have_item) {
            char item_day[9];
            int n = format_report(&item, item_day, rows, sizeof(rows));
            if (n < 0) { LOG_ERROR("yolo events: invalid daily report metadata"); continue; }
            if (used && (strcmp(day, item_day) || batch_epoch != item_epoch || used + (size_t)n > sizeof(batch))) {
                if (flush_report_retry(day, batch, used, batch_epoch)) return NULL;
                used = 0;
            }
            if (!used) { memcpy(day, item_day, sizeof(day)); batch_epoch = item_epoch; deadline = monotonic_ms() + 1000u; }
            memcpy(batch + used, rows, (size_t)n); used += (size_t)n;
        } else if (stopping) break;
    }
    if (used) (void)flush_report_retry(day, batch, used, batch_epoch);
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
    events.queue[index].camera[YOLO_EVENT_CAMERA_LEN - 1] = '\0';
    events.queue_epoch[index] = atomic_load(&report_epoch);
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
