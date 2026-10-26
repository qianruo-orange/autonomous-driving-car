/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <sys/statvfs.h>
#include <string.h>
#include <dirent.h>
/**
 * http_api_system.c — 系统监控 API（CPU / 内存 / 磁盘 / 温度 / GPU / NPU）。
 */

#include "http/http_internal.h"
#include "core/version.h"

#define MAX_CORES     16
#define JSON_BUF_SIZE 8192

void http_system_api(app_ctx_t *app, int fd)
{
    (void)app;
    char json[JSON_BUF_SIZE]; int off = 0;

    /* CPU 负载 */
    double load1=0, load5=0, load15=0;
    FILE *fp = fopen("/proc/loadavg", "r");
    if (fp) { fscanf(fp, "%lf %lf %lf", &load1, &load5, &load15); fclose(fp); }

    /* CPU 总体统计 */
    long cpu_user=0, cpu_sys=0, cpu_idle=0, cpu_total=0;
    /* CPU 逐核统计（动态，最多 MAX_CORES 核） */
    long core_user[MAX_CORES]={0}, core_sys[MAX_CORES]={0}, core_idle[MAX_CORES]={0};
    int  num_cores = 0;

    fp = fopen("/proc/stat", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "cpu", 3) != 0) continue;
            long u=0, n=0, s=0, id=0;
            if (line[3] == ' ') {
                /* 总体行 "cpu  ..." */
                sscanf(line + 4, "%ld %ld %ld %ld", &u, &n, &s, &id);
                cpu_user = u; cpu_sys = s; cpu_idle = id;
                cpu_total = u + n + s + id;
            } else {
                /* 逐核行 "cpuN ..." */
                int c = atoi(line + 3);
                if (c < 0 || c >= MAX_CORES) continue;
                sscanf(line + 4, "%ld %ld %ld %ld", &u, &n, &s, &id);
                core_user[c] = u;
                core_sys[c] = s;
                core_idle[c] = id + n;
                if (c + 1 > num_cores) num_cores = c + 1;
            }
        }
        fclose(fp);
    }

    /* per-core CPU frequency (MHz): scaling_cur_freq (kHz) -> MHz; fallback cpuinfo_cur_freq */
    long core_freq[MAX_CORES] = {0};
    for (int c = 0; c < num_cores; c++) {
        char fp[128];
        snprintf(fp, sizeof(fp), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", c);
        FILE *ff = fopen(fp, "r");
        if (ff) { long khz = 0; if (fscanf(ff, "%ld", &khz) == 1 && khz > 0) core_freq[c] = khz / 1000; fclose(ff); }
        else {
            snprintf(fp, sizeof(fp), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_cur_freq", c);
            ff = fopen(fp, "r");
            if (ff) { long khz = 0; if (fscanf(ff, "%ld", &khz) == 1 && khz > 0) core_freq[c] = khz / 1000; fclose(ff); }
        }
    }

    /* memory */
    long mem_total = http_read_key_long("/proc/meminfo", "MemTotal:");
    long mem_avail = http_read_key_long("/proc/meminfo", "MemAvailable:");
    int mem_pct = (mem_total > 0) ? (int)(100 - (mem_avail * 100 / mem_total)) : 0;

    /* 磁盘 */
    long disk_total = 0, disk_avail = 0;
    struct statvfs st;
    if (statvfs("/", &st) == 0) {
        disk_total = (long)st.f_blocks * st.f_frsize / 1024 / 1024;
        disk_avail = (long)st.f_bavail * st.f_frsize / 1024 / 1024;
    }

    /* 温度 */
    int temp = 0;
    fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (fp) { fscanf(fp, "%d", &temp); fclose(fp); temp /= 1000; }

    /* NPU 逐核负载（RKNPU 全系兼容：RK3588 三核 / RK3576 双核 / RK3566 等单核；
     * 逐核定位 "CoreN: x%" 自适应解析，核数按实际解析结果统计（npu_count），
     * 前端按 npu_count 动态渲染；任意核缺失保持 -1，前端显示 N/A */
    int npu_cores[3] = {-1, -1, -1};
    int npu_count = 0;
    fp = fopen("/sys/kernel/debug/rknpu/load", "r");
    if (fp) {
        char buf[256];
        while (fgets(buf, sizeof(buf), fp)) {
            for (int c = 0; c < 3; c++) {
                if (npu_cores[c] >= 0) continue;
                char key[16];
                snprintf(key, sizeof(key), "Core%d:", c);
                const char *p = strstr(buf, key);
                if (p && sscanf(p + strlen(key), " %d%%", &npu_cores[c]) == 1
                    && c + 1 > npu_count)
                    npu_count = c + 1;
            }
            /* 无 Core 前缀的单值格式："NPU load: x%"（单核 SoC） */
            if (npu_count == 0) {
                int v;
                if (sscanf(buf, "NPU load: %d%%", &v) == 1) {
                    npu_cores[0] = v;
                    npu_count = 1;
                }
            }
        }
        fclose(fp);
    }

    /* GPU (Mali) — 自适应查找 devfreq 节点：RK3588=fb000000.gpu / RK3576=27800000.gpu 等。
       优先读 load（RK3588 等暴露负载的驱动）；RK356x/RK3576 驱动未暴露 load 时，
       回退读 cur_freq 作为 GPU 频率指标（Hz→MHz），保证 GPU 卡片在所有平台都有真实信息 */
    long gpu_load = -1;
    long gpu_freq = -1;
    DIR *dev = opendir("/sys/class/devfreq");
    if (dev) {
        struct dirent *de;
        while ((de = readdir(dev))) {
            if (strstr(de->d_name, "gpu") == NULL) continue;
            char base[256];
            snprintf(base, sizeof(base), "/sys/class/devfreq/%s", de->d_name);
            FILE *gfp;
            char gp[320];
            snprintf(gp, sizeof(gp), "%s/load", base);
            gfp = fopen(gp, "r");
            if (gfp) {
                char buf[64];
                if (fgets(buf, sizeof(buf), gfp)) sscanf(buf, "%ld", &gpu_load);
                fclose(gfp);
            }
            if (gpu_load < 0) {   /* 无 load 文件时回退读当前频率 */
                snprintf(gp, sizeof(gp), "%s/cur_freq", base);
                gfp = fopen(gp, "r");
                if (gfp) {
                    long f = -1;
                    if (fscanf(gfp, "%ld", &f) == 1 && f > 0) gpu_freq = f / 1000000;
                    fclose(gfp);
                }
            }
            if (gpu_load >= 0 || gpu_freq >= 0) break;
        }
        closedir(dev);
    }

    /* 组装 JSON（使用边界检查宏） */
    JSON_ADD(json, off, "{"
        "\"load\":[%.2f,%.2f,%.2f],"
        "\"cpu_user\":%ld,\"cpu_sys\":%ld,\"cpu_idle\":%ld,\"cpu_total\":%ld,",
        load1, load5, load15,
        cpu_user, cpu_sys, cpu_idle, cpu_total);

    JSON_ADD(json, off, "\"cpu_cores\":[");
    for (int i = 0; i < num_cores; i++)
        JSON_ADD(json, off, "%s{\"u\":%ld,\"s\":%ld,\"i\":%ld}",
            i > 0 ? "," : "", core_user[i], core_sys[i], core_idle[i]);
    JSON_ADD(json, off, "],");
    JSON_ADD(json, off, "\"cpu_freqs\":[");
    for (int i = 0; i < num_cores; i++)
        JSON_ADD(json, off, "%s%ld", i > 0 ? "," : "", core_freq[i]);
    JSON_ADD(json, off, "],");

    JSON_ADD(json, off, "\"npu_count\":%d,\"npu_cores\":[", npu_count);
    for (int c = 0; c < npu_count; c++)
        JSON_ADD(json, off, "%s%d", c > 0 ? "," : "", npu_cores[c]);
    JSON_ADD(json, off, "],");

    JSON_ADD(json, off,
        "\"mem_pct\":%d,\"mem_total\":%ld,\"mem_avail\":%ld,"
        "\"disk_total\":%ld,\"disk_avail\":%ld,"
        "\"temp\":%d,"
        "\"gpu\":%ld,\"gpu_freq\":%ld,"
        "\"app_version\":\"%s\",\"git_commit\":\"%s\",\"build_date\":\"%s\""
        "}",
        mem_pct, mem_total, mem_avail,
        disk_total, disk_avail,
        temp,
        gpu_load, gpu_freq,
        APP_VERSION, APP_GIT_COMMIT, APP_BUILD_DATE);

    http_ok_json(fd, json, (size_t)off);
}
