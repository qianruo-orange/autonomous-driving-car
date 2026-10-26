/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_cpufreq.c — CPU 频率配置 API。
 *   GET  /api/cpu_freq  读取当前 governor、可用 governor 列表、可用频率、当前 min/max。
 *   POST /api/cpu_freq  设置 governor 及可选的 min/max 频率（写入 sysfs，所有 CPU 核心同步）。
 */

#include "http/http_internal.h"
#include <dirent.h>

#define CPUFREQ_PATH    "/sys/devices/system/cpu/cpu0/cpufreq"
#define CPU_PATH_FMT    "/sys/devices/system/cpu/cpu%d/cpufreq/%s"
#define MAX_FREQS       32
#define MAX_GOVS        16
#define JSON_SZ         4096

/* ---- 辅助：读取 sysfs 单行文本文件 ---- */
static int sysfs_read(const char *path, char *buf, size_t sz)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(buf, (int)sz, fp)) { fclose(fp); return -1; }
    fclose(fp);
    /* 去尾部换行 */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' '))
        buf[--len] = '\0';
    return 0;
}

/* ---- 辅助：写入 sysfs 文件 ---- */
static int sysfs_write(const char *path, const char *val)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    int r = fputs(val, fp);
    fclose(fp);
    return (r >= 0) ? 0 : -1;
}

/* ---- 辅助：写入所有 CPU 核心的 sysfs 文件 ---- */
static void sysfs_write_all_cpus(const char *file, const char *val)
{
    DIR *d = opendir("/sys/devices/system/cpu");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        int cpu_id = -1;
        if (sscanf(de->d_name, "cpu%d", &cpu_id) != 1) continue;
        char path[256];
        snprintf(path, sizeof(path), CPU_PATH_FMT, cpu_id, file);
        sysfs_write(path, val);
    }
    closedir(d);
}

/* ---- GET /api/cpu_freq ---- */
void http_cpufreq_get(app_ctx_t *app, int fd, const char *method,
                      const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    char json[JSON_SZ];
    int off = 0;
    char buf[512];

    /* 当前 governor */
    char governor[64] = "";
    snprintf(buf, sizeof(buf), "%s/scaling_governor", CPUFREQ_PATH);
    if (sysfs_read(buf, governor, sizeof(governor)) != 0)
        strcpy(governor, "unknown");

    /* 可用 governor 列表 */
    char gov_list[256] = "";
    snprintf(buf, sizeof(buf), "%s/scaling_available_governors", CPUFREQ_PATH);
    sysfs_read(buf, gov_list, sizeof(gov_list));

    /* 可用频率列表 */
    char freq_list[512] = "";
    snprintf(buf, sizeof(buf), "%s/scaling_available_frequencies", CPUFREQ_PATH);
    sysfs_read(buf, freq_list, sizeof(freq_list));

    /* 当前 min/max 频率 (kHz) */
    char min_freq[32] = "0", max_freq[32] = "0";
    snprintf(buf, sizeof(buf), "%s/scaling_min_freq", CPUFREQ_PATH);
    sysfs_read(buf, min_freq, sizeof(min_freq));
    snprintf(buf, sizeof(buf), "%s/scaling_max_freq", CPUFREQ_PATH);
    sysfs_read(buf, max_freq, sizeof(max_freq));

    /* 当前实际频率 (kHz) */
    char cur_freq[32] = "0";
    snprintf(buf, sizeof(buf), "%s/scaling_cur_freq", CPUFREQ_PATH);
    sysfs_read(buf, cur_freq, sizeof(cur_freq));

    /* 构建 JSON */
    JSON_ADD(json, off, "{");
    JSON_ADD(json, off, "\"governor\":\"%s\",", governor);

    /* governors 数组 */
    JSON_ADD(json, off, "\"governors\":[");
    {
        char tmp[256];
        strncpy(tmp, gov_list, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        int first = 1;
        char *tok = strtok(tmp, " ");
        while (tok) {
            JSON_ADD(json, off, "%s\"%s\"", first ? "" : ",", tok);
            first = 0;
            tok = strtok(NULL, " ");
        }
    }
    JSON_ADD(json, off, "],");

    /* frequencies 数组 (kHz) */
    JSON_ADD(json, off, "\"frequencies\":[");
    {
        char tmp[512];
        strncpy(tmp, freq_list, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        int first = 1;
        char *tok = strtok(tmp, " ");
        while (tok) {
            JSON_ADD(json, off, "%s%s", first ? "" : ",", tok);
            first = 0;
            tok = strtok(NULL, " ");
        }
    }
    JSON_ADD(json, off, "],");

    JSON_ADD(json, off, "\"min_freq\":%s,", min_freq);
    JSON_ADD(json, off, "\"max_freq\":%s,", max_freq);
    JSON_ADD(json, off, "\"cur_freq\":%s", cur_freq);
    JSON_ADD(json, off, "}");

    http_ok_json(fd, json, (size_t)off);
}

/* ---- POST /api/cpu_freq ---- */
void http_cpufreq_post(app_ctx_t *app, int fd, const char *method,
                       const char *uri, const char *req)
{
    (void)app; (void)uri;
    if (strcmp(method, "POST") != 0) {
        http_err(fd, 405, "Method Not Allowed", NULL);
        return;
    }

    http_form_field_t fields[8];
    int nf = http_form_parse(req, fields, 8);

    const char *governor  = http_form_find(fields, nf, "governor");
    const char *min_freq  = http_form_find(fields, nf, "min_freq");
    const char *max_freq  = http_form_find(fields, nf, "max_freq");

    char json[512];
    int off = 0;
    int changed = 0;

    /* 设置 governor */
    if (governor && governor[0]) {
        /* 校验：只允许已知 governor 名 */
        const char *allowed[] = {"performance","ondemand","schedutil",
                                 "powersave","userspace","conservative",NULL};
        int valid = 0;
        for (int i = 0; allowed[i]; i++)
            if (strcmp(governor, allowed[i]) == 0) { valid = 1; break; }
        if (!valid) {
            http_err(fd, 400, "Bad Request", "{\"error\":\"invalid governor\"}");
            return;
        }
        sysfs_write_all_cpus("scaling_governor", governor);
        changed++;
    }

    /* 设置 min_freq (kHz) */
    if (min_freq && min_freq[0]) {
        long v = strtol(min_freq, NULL, 10);
        if (v <= 0 || v > 5000000) {
            http_err(fd, 400, "Bad Request", "{\"error\":\"invalid min_freq\"}");
            return;
        }
        sysfs_write_all_cpus("scaling_min_freq", min_freq);
        changed++;
    }

    /* 设置 max_freq (kHz) */
    if (max_freq && max_freq[0]) {
        long v = strtol(max_freq, NULL, 10);
        if (v <= 0 || v > 5000000) {
            http_err(fd, 400, "Bad Request", "{\"error\":\"invalid max_freq\"}");
            return;
        }
        sysfs_write_all_cpus("scaling_max_freq", max_freq);
        changed++;
    }

    if (changed == 0) {
        http_err(fd, 400, "Bad Request", "{\"error\":\"no parameters\"}");
        return;
    }

    JSON_ADD(json, off, "{\"result\":\"ok\",\"changed\":%d}", changed);
    http_ok_json(fd, json, (size_t)off);
}
