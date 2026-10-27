/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Large report generation must never block the HTTP reactor or its watchdog.
 * A successful start transfers the socket to this detached worker. */
#define _GNU_SOURCE
#include "yolo/yolo_events.h"
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#ifndef DETECTION_DOWNLOAD_IDLE_MS
#define DETECTION_DOWNLOAD_IDLE_MS 30000u
#endif
#ifndef DETECTION_DOWNLOAD_TOTAL_MS
#define DETECTION_DOWNLOAD_TOTAL_MS 300000u
#endif

typedef struct {
    int fd, archive;
    char relative[34];
    void (*on_close)(int);
} detection_download_t;

static pthread_mutex_t detection_download_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned detection_download_active;

static uint64_t download_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + now.tv_nsec / 1000000u;
}

typedef struct { uint64_t started, progress; } download_clock_t;

static int download_send(int fd, const void *bytes, size_t length, download_clock_t *clock)
{
    size_t offset = 0;
    while (offset < length) {
        uint64_t now = download_now_ms();
        uint64_t idle = now - clock->progress, total = now - clock->started;
        if (idle >= DETECTION_DOWNLOAD_IDLE_MS || total >= DETECTION_DOWNLOAD_TOTAL_MS) {
            errno = ETIMEDOUT; return -1;
        }
        ssize_t written = send(fd, (const char *)bytes + offset, length - offset, MSG_NOSIGNAL);
        if (written > 0) {
            offset += (size_t)written;
            clock->progress = download_now_ms();
            continue;
        }
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            uint64_t remaining = DETECTION_DOWNLOAD_IDLE_MS - idle;
            if (remaining > DETECTION_DOWNLOAD_TOTAL_MS - total)
                remaining = DETECTION_DOWNLOAD_TOTAL_MS - total;
            struct pollfd poller = {.fd = fd, .events = POLLOUT};
            int ready = poll(&poller, 1, (int)remaining);
            if (ready < 0 && errno == EINTR) continue;
            if (!ready) { errno = ETIMEDOUT; return -1; }
            if (ready < 0 || (poller.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static void download_error(int fd, int error, download_clock_t *clock)
{
    int status = error == ENOENT ? 404 : error == EINVAL || error == ELOOP ? 400 :
        error == EAGAIN ? 409 : error == EFBIG ? 413 : 500;
    const char *reason = status == 404 ? "Not Found" : status == 400 ? "Bad Request" :
        status == 409 ? "Conflict" : status == 413 ? "Payload Too Large" : "Internal Server Error";
    const char *message = status == 404 ? "No detection reports found" :
        status == 409 ? "Detection reports changed; please retry" :
        status == 413 ? "Detection reports are too large" : "Unable to export detection reports";
    char header[512];
    int length = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n%s",
        status, reason, strlen(message), message);
    if (length > 0 && (size_t)length < sizeof(header))
        (void)download_send(fd, header, (size_t)length, clock);
}

static void *detection_download_worker(void *argument)
{
    detection_download_t *job = argument;
    (void)nice(10);
    int flags = fcntl(job->fd, F_GETFL);
    if (flags < 0 || fcntl(job->fd, F_SETFL, flags | O_NONBLOCK) < 0) goto done;
    int source = job->archive ? yolo_events_pack_reports(NULL) :
        yolo_events_export_report(job->relative);
    int export_error = errno;
    uint64_t started = download_now_ms();
    download_clock_t clock = {.started = started, .progress = started};
    if (source < 0) {
        download_error(job->fd, export_error, &clock);
        goto done;
    }
    struct stat info;
    if (fstat(source, &info) || !S_ISREG(info.st_mode) || info.st_size < 0 || lseek(source, 0, SEEK_SET) < 0) {
        close(source);
        download_error(job->fd, EIO, &clock);
        goto done;
    }
    const char *filename = job->archive ? "detections_pack.tar.gz" : job->relative + 9;
    const char *mime = job->archive ? "application/gzip" :
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
    char header[512];
    int header_length = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %" PRIuMAX "\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        mime, (uintmax_t)info.st_size, filename);
    if (header_length <= 0 || (size_t)header_length >= sizeof(header) ||
        download_send(job->fd, header, (size_t)header_length, &clock)) {
        close(source); goto done;
    }
    char buffer[65536];
    off_t remaining = info.st_size;
    while (remaining > 0) {
        size_t requested = remaining > (off_t)sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        ssize_t length = read(source, buffer, requested);
        if (length < 0 && errno == EINTR) continue;
        if (length <= 0 || download_send(job->fd, buffer, (size_t)length, &clock)) break;
        remaining -= length;
    }
    close(source);
done:
    close(job->fd);
    pthread_mutex_lock(&detection_download_lock);
    detection_download_active--;
    pthread_mutex_unlock(&detection_download_lock);
    if (job->on_close) job->on_close(job->fd);
    free(job);
    return NULL;
}

int http_detection_download_start(int fd, const char *uri, void (*on_close)(int))
{
    const char *prefix = "/detectionfile/";
    int archive = uri && !strcmp(uri, "/api/detections/reports/pack");
    const char *relative = uri && !strncmp(uri, prefix, strlen(prefix)) ? uri + strlen(prefix) : NULL;
    if (fd < 0 || (!archive && (!relative || strlen(relative) >= sizeof(((detection_download_t *)0)->relative) ||
                              !yolo_report_rel_valid(relative)))) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&detection_download_lock);
    if (detection_download_active) {
        pthread_mutex_unlock(&detection_download_lock); errno = EBUSY; return -1;
    }
    detection_download_active++;
    pthread_mutex_unlock(&detection_download_lock);
    detection_download_t *job = calloc(1, sizeof(*job));
    int error = ENOMEM;
    if (!job) goto failed;
    job->fd = fd; job->archive = archive; job->on_close = on_close;
    if (relative) memcpy(job->relative, relative, strlen(relative) + 1);
    pthread_attr_t attributes;
    error = pthread_attr_init(&attributes);
    if (!error) {
        error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        if (!error) error = pthread_create(&thread, &attributes, detection_download_worker, job);
        pthread_attr_destroy(&attributes);
        if (!error) return 0;
    }
    free(job);
failed:
    pthread_mutex_lock(&detection_download_lock);
    detection_download_active--;
    pthread_mutex_unlock(&detection_download_lock);
    errno = error;
    return -1;
}
