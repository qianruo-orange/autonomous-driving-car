/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOG_H
#define LOG_H

/**
 * 初始化日志系统。
 * @param arg  app_ctx_t 上下文；未配置日志目录时使用 PATH_LOGS。
 *
 * Info/error logs include their creation timestamp and millisecond precision:
 *   dir/YYYYMMDD/log_info_YYYYMMDD_HHMMSS_mmm.log
 *   dir/YYYYMMDD/log_error_YYYYMMDD_HHMMSS_mmm.log
 * A collision suffix preserves files created within the same millisecond.
 */
int log_init(void *arg);

/* 关闭日志文件 */
void log_close(void *arg);

void _log_info(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void _log_error(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOG_INFO(fmt, ...)  _log_info(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) _log_error(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif /* LOG_H */
