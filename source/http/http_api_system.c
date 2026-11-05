/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <sys/statvfs.h>
#include <sys/reboot.h>
#include <string.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <unistd.h>
/**
 * http_api_system.c — 系统状态、网络统计和电源控制 API。
 */

#include "http/http_internal.h"
#include "video/rga_preprocess.h"
#include "yolo/rknn_yolo.h"
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

    /* 本工程只针对 RK3576：NPU 固定双核，按 Core0/Core1 两行解析，无核数自适应 */
    int npu_cores[2] = { -1, -1 };
    const int npu_count = npu_core_count();
    fp = fopen("/sys/kernel/debug/rknpu/load", "r");
    if (fp) {
        char buf[256];
        while (fgets(buf, sizeof(buf), fp)) {
            for (int c = 0; c < npu_count; c++) {
                char key[16];
                snprintf(key, sizeof(key), "Core%d:", c);
                const char *p = strstr(buf, key);
                if (p) sscanf(p + strlen(key), " %d%%", &npu_cores[c]);
            }
        }
        fclose(fp);
    }

    /* RGA（RGA2E 双核）负载：没有 devfreq/load 文件可读，占用率由预处理模块
       按核计时统计（1 秒窗口）。字段结构与 NPU 一致：rga_count + rga_cores[]，
       前端据此动态渲染；RGA 未启用时 count=0，前端显示 N/A。 */
    int rga_cores[2] = { -1, -1 };
    int rga_count = 0;
    unsigned long long rga_jobs = 0;
    rga_preprocess_load(rga_cores, &rga_jobs);
    if (rga_cores[0] >= 0) rga_count = 1;
    if (rga_cores[1] >= 0) rga_count = 2;

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
        "\"rga_count\":%d,\"rga_cores\":[%d,%d],\"rga_jobs\":%llu,"
        "\"app_version\":\"%s\",\"git_commit\":\"%s\",\"build_date\":\"%s\""
        "}",
        mem_pct, mem_total, mem_avail,
        disk_total, disk_avail,
        temp,
        rga_count, rga_cores[0], rga_cores[1], rga_jobs,
        APP_VERSION, APP_GIT_COMMIT, APP_BUILD_DATE);

    http_ok_json(fd, json, (size_t)off);
}

/* Network status endpoints belong with the system monitor API. */
void http_network_ifaces(app_ctx_t *app,int fd,const char *method,const char *uri,const char *req_buf)
{
    (void)app;(void)method;(void)uri;(void)req_buf;
    char json[1024];int offset=0,first=1;JSON_ADD(json,offset,"[");
    struct ifaddrs *interfaces=NULL;
    if(getifaddrs(&interfaces)==0){
        for(struct ifaddrs *item=interfaces;item;item=item->ifa_next){
            if(!item->ifa_name||!item->ifa_addr||item->ifa_addr->sa_family!=AF_PACKET||
               (item->ifa_flags&IFF_LOOPBACK))continue;
            JSON_ADD(json,offset,"%s\"%s\"",first?"":",",item->ifa_name);first=0;
        }
        freeifaddrs(interfaces);
    }
    JSON_ADD(json,offset,"]");http_ok_json(fd,json,(size_t)offset);
}

void http_network_api(app_ctx_t *app,int fd)
{
    (void)app;const char *interfaces[]={NET_IFACE_ETH,NET_IFACE_WLAN};
    char json[512];int offset=snprintf(json,sizeof(json),"{");
    FILE *fp=fopen("/proc/net/dev","r");
    if(!fp){
        JSON_ADD(json,offset,"\"%s\":{\"rx\":0,\"tx\":0},\"%s\":{\"rx\":0,\"tx\":0}}",interfaces[0],interfaces[1]);
        http_ok_json(fd,json,(size_t)offset);return;
    }
    char line[256];int found=0;
    while(fgets(line,sizeof(line),fp)){
        char name[32];unsigned long rx,tx;
        if(sscanf(line," %31[^:]: %lu %*u %*u %*u %*u %*u %*u %*u %lu",name,&rx,&tx)!=3)continue;
        if(strcmp(name,interfaces[0])&&strcmp(name,interfaces[1]))continue;
        JSON_ADD(json,offset,"%s\"%s\":{\"rx\":%lu,\"tx\":%lu}",found?",":"",name,rx,tx);found++;
    }
    fclose(fp);
    for(int i=0,need=found>0;i<2;i++)if(!strstr(json,interfaces[i])){
        JSON_ADD(json,offset,"%s\"%s\":{\"rx\":0,\"tx\":0}",need?",":"",interfaces[i]);need=1;
    }
    JSON_ADD(json,offset,"}");http_ok_json(fd,json,(size_t)offset);
}

static void reboot_action(int fd,int power_off)
{
    char html[512];int length=snprintf(html,sizeof(html),
        "<html><body style='font-family:Segoe UI;background:#0d1117;color:#c9d1d9;padding:40px;text-align:center'>"
        "<h2 style='color:%s'> %s...</h2><p>%s</p></body></html>",
        power_off?"#da3633":"#58a6ff",power_off?"正在关机":"正在重启",power_off?"设备即将关闭":"设备将在几秒后重启");
    http_send_response(fd,200,"OK","text/html; charset=utf-8",html,(size_t)length);
    sync();reboot(power_off?RB_POWER_OFF:RB_AUTOBOOT);
}

void http_reboot(app_ctx_t *app,int fd){(void)app;reboot_action(fd,0);}
void http_shutdown(app_ctx_t *app,int fd){(void)app;reboot_action(fd,1);}
