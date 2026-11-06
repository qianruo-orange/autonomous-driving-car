/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LOG_H
#define LOG_H

/**
 * 初始化日志系统。
 * @param arg  app_ctx_t 上下文；未配置日志目录时使用 PATH_LOGS。
 *
 * 日志目录一级平铺，一天一个文件，按天追加：
 *   dir/log_info_YYYYmmdd.log
 *   dir/log_err_YYYYmmdd.log
 * 超过 10MB 时当天文件轮转为 .1；文件被 Web 清空/删除后，下一次写日志
 * 会重新创建同名文件，无需重启服务。
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
