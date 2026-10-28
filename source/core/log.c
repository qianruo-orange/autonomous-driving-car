/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#include "core/log.h"
#include "core/common.h"
#include "core/version.h"

typedef struct {
    FILE    *fp_info;
    FILE    *fp_error;
    char     dir[256];
    char     date[16];
    char     info_path[PATH_MAX], error_path[PATH_MAX];
    pthread_mutex_t mutex;
} log_ctx_t;

static log_ctx_t g_log = { .mutex = PTHREAD_MUTEX_INITIALIZER };

#define LOG_MAX_SIZE (10 * 1024 * 1024)  /* 10MB */

/* Exclusive creation preserves old logs even on rapid restarts or clock
   adjustments. The timestamp remains meaningful outside the date directory. */
static void open_log_channel(int is_error)
{
    FILE **fpp = is_error ? &g_log.fp_error : &g_log.fp_info;
    char *active_path = is_error ? g_log.error_path : g_log.info_path;
    if (*fpp && *fpp != stderr) fclose(*fpp);
    *fpp = stderr;
    active_path[0] = '\0';
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    char stamp[32], name[128], path[PATH_MAX];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    for (int index = 0; index < 10000; index++) {
        if (!index)
            snprintf(name, sizeof(name), "%s_%s_%s_%03ld.log", "log",
                     is_error ? "error" : "info", stamp, tv.tv_usec / 1000);
        else
            snprintf(name, sizeof(name), "%s_%s_%s_%03ld_%d.log", "log",
                     is_error ? "error" : "info", stamp, tv.tv_usec / 1000, index);
        int length = snprintf(path, sizeof(path), "%s/%s/%s", g_log.dir, g_log.date, name);
        if (length < 0 || (size_t)length >= sizeof(path)) return;
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) { if (errno == EEXIST) continue; return; }
        FILE *fp = fdopen(fd, "a");
        if (!fp) { close(fd); unlink(path); return; }
        *fpp = fp;
        safe_strncpy(active_path, PATH_MAX, path);
        return;
    }
}

/* Deletion and size rotation open a fresh timestamped file. Existing logs and
   legacy .log.1 backups are never renamed or overwritten. */
static void rotate_check(int is_error)
{
    FILE **fpp = is_error ? &g_log.fp_error : &g_log.fp_info;
    const char *path = is_error ? g_log.error_path : g_log.info_path;
    if (!*fpp || *fpp == stderr || !*path) return;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size > LOG_MAX_SIZE)
        open_log_channel(is_error);
}

static void ensure_date(void)
{
    time_t now = time(NULL);
    struct tm tm_buf, *tm = localtime_r(&now, &tm_buf);
    char cur[16]; strftime(cur, sizeof(cur), "%Y%m%d", tm);
    if (strcmp(cur, g_log.date) == 0) return;

    safe_strncpy(g_log.date, sizeof(g_log.date), cur);
    /* 二级目录：按日期存放 logs/YYYYMMDD/ */
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/%s", g_log.dir, g_log.date);
    mkdir(subdir, 0755);

    open_log_channel(0);
    open_log_channel(1);
}

void log_init(const char *dir)
{
    pthread_mutex_lock(&g_log.mutex);
    safe_strncpy(g_log.dir, sizeof(g_log.dir), dir);
    mkdir(dir, 0755);
    g_log.date[0] = '\0';
    ensure_date();
    pthread_mutex_unlock(&g_log.mutex);
}

void log_close(void)
{
    pthread_mutex_lock(&g_log.mutex);
    if (g_log.fp_info  && g_log.fp_info  != stderr) fclose(g_log.fp_info);
    if (g_log.fp_error && g_log.fp_error != stderr) fclose(g_log.fp_error);
    g_log.fp_info = g_log.fp_error = NULL;
    pthread_mutex_unlock(&g_log.mutex);
}

static void log_msg(FILE *fp, const char *file, int line,
                    const char *level, const char *fmt, va_list ap)
{
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm tm_buf, *tm = localtime_r(&tv.tv_sec, &tm_buf);
    char buf[4096];
    int off = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03ld] %-5s %s:%d ",
                       tm->tm_hour, tm->tm_min, tm->tm_sec, tv.tv_usec/1000, level, file, line);
    /* snprintf 出错（off<0）或前缀写满缓冲区时，钳制偏移，防止
       sizeof(buf)-off 溢出导致 vsnprintf 越界写 */
    if (off < 0) off = 0;
    if ((size_t)off >= sizeof(buf)) off = (int)sizeof(buf) - 1;
    vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, ap);
    fprintf(fp, "%s\n", buf); fflush(fp);
}

void _log_info(const char *file, int line, const char *fmt, ...)
{
    pthread_mutex_lock(&g_log.mutex); ensure_date();
    rotate_check(0);
    va_list ap; va_start(ap, fmt);
    log_msg(g_log.fp_info, file, line, "INFO", fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_log.mutex);
}

void _log_error(const char *file, int line, const char *fmt, ...)
{
    pthread_mutex_lock(&g_log.mutex); ensure_date();
    rotate_check(1);
    va_list ap; va_start(ap, fmt);
    log_msg(g_log.fp_error, file, line, "ERROR", fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&g_log.mutex);
}
