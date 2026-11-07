/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_bluetooth.c �?配置页「蓝牙连接」卡片的 HTTP 接口�? *
 *   GET  /api/bluetooth/status   适配器状�?+ 最近一次动作结�? *   GET  /api/bluetooth/devices  已发�?已配对设备列�? *   POST /api/bluetooth/adapter  powered / discoverable / pairable / name
 *   POST /api/bluetooth/scan     on=on|off 开�?停止扫描
 *   POST /api/bluetooth/device   address=..&action=pair|connect|disconnect|trust|untrust|remove
 *
 * 配对与连接会阻塞十几秒到几十秒（要等对端响应），必须离开 HTTP 反应堆线程：
 * 接口立即返回 {"ok":true,"accepted":true}，前端轮�?status/devices 看结果，
 * 动作结果记录�?status �?last 字段里�? */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "http/http_internal.h"
#include "bluetooth/bt_manager.h"
#include "core/log.h"

#define BT_JSON_MAX  (64 * 1024)

/* ---- 最近一次设备动�?---- */
typedef struct {
    int  have;
    int  ok;
    char action[16];
    char address[BT_ADDR_MAX];
    char error[256];
    time_t at;
} bt_last_t;

static bt_last_t        g_last;
static pthread_mutex_t  g_last_lock = PTHREAD_MUTEX_INITIALIZER;

static void last_set(const char *action, const char *address, int ok, const char *error)
{
    pthread_mutex_lock(&g_last_lock);
    g_last.have = 1;
    g_last.ok = ok;
    snprintf(g_last.action, sizeof(g_last.action), "%s", action ? action : "");
    snprintf(g_last.address, sizeof(g_last.address), "%s", address ? address : "");
    snprintf(g_last.error, sizeof(g_last.error), "%s", error ? error : "");
    g_last.at = time(NULL);
    pthread_mutex_unlock(&g_last_lock);
}

static void last_format(char *out, size_t size)
{
    pthread_mutex_lock(&g_last_lock);
    if (!g_last.have) {
        snprintf(out, size, "null");
    } else {
        char esc_err[600];
        http_json_escape(g_last.error, esc_err, sizeof(esc_err));
        snprintf(out, size,
                 "{\"action\":\"%s\",\"address\":\"%s\",\"ok\":%s,\"error\":\"%s\",\"at\":%lld}",
                 g_last.action, g_last.address, g_last.ok ? "true" : "false",
                 esc_err, (long long)g_last.at);
    }
    pthread_mutex_unlock(&g_last_lock);
}

/* ---- 设备动作 worker ---- */
typedef struct {
    char address[BT_ADDR_MAX];
    char action[16];
} bt_job_t;

static void *bt_job_worker(void *arg)
{
    bt_job_t *job = arg;
    int ok = bt_device_action(job->address, job->action) == 0;
    last_set(job->action, job->address, ok, ok ? "" : bt_last_error());
    if (ok) LOG_INFO("bluetooth: %s %s ok", job->action, job->address);
    free(job);
    return NULL;
}

/* ---- 响应构�?---- */

static void reply_unavailable(int fd)
{
    char esc[600], json[1024];
    http_json_escape(bt_last_error(), esc, sizeof(esc));
    int n = snprintf(json, sizeof(json),
                     "{\"ok\":true,\"available\":false,\"error\":\"%s\"}", esc);
    http_ok_json(fd, json, (size_t)n);
}

static void reply_status(int fd)
{
    bt_adapter_t a;
    if (bt_snapshot(&a, NULL, 0) < 0 || !a.available) {
        const char *msg = bt_last_error();
        if (!a.available && !msg[0]) msg = "no Bluetooth adapter";
        char esc[600], json[1024];
        http_json_escape(msg, esc, sizeof(esc));
        int n = snprintf(json, sizeof(json),
                         "{\"ok\":true,\"available\":false,\"error\":\"%s\"}", esc);
        http_ok_json(fd, json, (size_t)n);
        return;
    }

    char last[768];
    last_format(last, sizeof(last));
    char esc_name[300], esc_alias[300];
    http_json_escape(a.name, esc_name, sizeof(esc_name));
    http_json_escape(a.alias, esc_alias, sizeof(esc_alias));

    char json[1536];
    int n = snprintf(json, sizeof(json),
                     "{\"ok\":true,\"available\":true,\"powered\":%s,\"discoverable\":%s,"
                     "\"pairable\":%s,\"discovering\":%s,\"address\":\"%s\","
                     "\"name\":\"%s\",\"alias\":\"%s\",\"device_count\":%d,\"last\":%s}",
                     a.powered ? "true" : "false",
                     a.discoverable ? "true" : "false",
                     a.pairable ? "true" : "false",
                     a.discovering ? "true" : "false",
                     a.address, esc_name, esc_alias, a.device_count, last);
    http_ok_json(fd, json, (size_t)n);
}

static int dev_cmp(const void *pa, const void *pb)
{
    const bt_device_t *a = pa, *b = pb;
    if (a->connected != b->connected) return b->connected - a->connected;
    if (a->paired != b->paired) return b->paired - a->paired;
    if (a->has_rssi && b->has_rssi && a->rssi != b->rssi) return b->rssi - a->rssi;
    return strcmp(a->address, b->address);
}

static void reply_devices(int fd)
{
    bt_device_t devs[BT_MAX_DEVICES];
    bt_adapter_t a;
    int rc = bt_snapshot(&a, devs, BT_MAX_DEVICES);
    if (rc < 0 || !a.available) {
        char esc[600], json[1024];
        http_json_escape(bt_last_error(), esc, sizeof(esc));
        int n = snprintf(json, sizeof(json),
                         "{\"ok\":true,\"available\":false,\"error\":\"%s\",\"devices\":[]}", esc);
        http_ok_json(fd, json, (size_t)n);
        return;
    }

    qsort(devs, (size_t)a.device_count, sizeof(devs[0]), dev_cmp);

    char *json = malloc(BT_JSON_MAX);
    if (!json) { http_err(fd, 500, "Error", "out of memory"); return; }
    int off = snprintf(json, BT_JSON_MAX, "{\"ok\":true,\"available\":true,\"discovering\":%s,\"devices\":[",
                       a.discovering ? "true" : "false");
    for (int i = 0; i < a.device_count && off > 0 && off < BT_JSON_MAX - 1024; i++) {
        bt_device_t *d = &devs[i];
        char esc_name[300], esc_alias[300], esc_icon[160];
        http_json_escape(d->name, esc_name, sizeof(esc_name));
        http_json_escape(d->alias, esc_alias, sizeof(esc_alias));
        http_json_escape(d->icon, esc_icon, sizeof(esc_icon));
        int n = snprintf(json + off, (size_t)(BT_JSON_MAX - off),
                         "%s{\"address\":\"%s\",\"name\":\"%s\",\"alias\":\"%s\",\"icon\":\"%s\","
                         "\"paired\":%s,\"connected\":%s,\"trusted\":%s,"
                         "\"has_rssi\":%s,\"rssi\":%d,\"class\":%u}",
                         i ? "," : "", d->address, esc_name, esc_alias, esc_icon,
                         d->paired ? "true" : "false",
                         d->connected ? "true" : "false",
                         d->trusted ? "true" : "false",
                         d->has_rssi ? "true" : "false", d->rssi, d->dev_class);
        if (n < 0 || off + n >= BT_JSON_MAX) break;
        off += n;
    }
    if (off > 0 && off < BT_JSON_MAX - 4) off += snprintf(json + off, (size_t)(BT_JSON_MAX - off), "]}");
    if (off < 0) { free(json); http_err(fd, 500, "Error", "encode failed"); return; }
    http_ok_json(fd, json, (size_t)off);
    free(json);
}

/* ---- 处理函数 ---- */

void http_bluetooth_status(app_ctx_t *app, int fd, const char *method,
                           const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    reply_status(fd);
}

void http_bluetooth_devices(app_ctx_t *app, int fd, const char *method,
                            const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri; (void)req;
    reply_devices(fd);
}

void http_bluetooth_adapter(app_ctx_t *app, int fd, const char *method,
                            const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri;
    char val[64];
    int applied = 0, failed = 0;
    char err[256] = "";

    if (http_form_get_param(req, "powered=", val, sizeof(val)) > 0) {
        applied++;
        if (bt_set_powered(!strcmp(val, "on")) < 0) { failed++; snprintf(err, sizeof(err), "%s", bt_last_error()); }
    }
    if (http_form_get_param(req, "discoverable=", val, sizeof(val)) > 0) {
        applied++;
        if (bt_set_discoverable(!strcmp(val, "on")) < 0) { failed++; snprintf(err, sizeof(err), "%s", bt_last_error()); }
    }
    if (http_form_get_param(req, "pairable=", val, sizeof(val)) > 0) {
        applied++;
        if (bt_set_pairable(!strcmp(val, "on")) < 0) { failed++; snprintf(err, sizeof(err), "%s", bt_last_error()); }
    }
    if (http_form_get_param(req, "name=", val, sizeof(val)) > 0 && val[0]) {
        applied++;
        if (bt_set_name(val) < 0) { failed++; snprintf(err, sizeof(err), "%s", bt_last_error()); }
    }

    if (!applied) { http_err(fd, 400, "Bad Request", "no adapter field supplied"); return; }

    char esc[600], json[800];
    http_json_escape(err, esc, sizeof(esc));
    int n = snprintf(json, sizeof(json), "{\"ok\":%s,\"applied\":%d,\"error\":\"%s\"}",
                     failed ? "false" : "true", applied, esc);
    http_ok_json(fd, json, (size_t)n);
}

void http_bluetooth_scan(app_ctx_t *app, int fd, const char *method,
                         const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri;
    char val[16] = "on";
    (void)http_form_get_param(req, "on=", val, sizeof(val));
    int on = strcmp(val, "off") != 0;
    if (bt_set_discovering(on) < 0) {
        char esc[600], json[800];
        http_json_escape(bt_last_error(), esc, sizeof(esc));
        int n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", esc);
        http_ok_json(fd, json, (size_t)n);
        return;
    }
    char json[96];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"discovering\":%s}", on ? "true" : "false");
    http_ok_json(fd, json, (size_t)n);
}

void http_bluetooth_device(app_ctx_t *app, int fd, const char *method,
                           const char *uri, const char *req)
{
    (void)app; (void)method; (void)uri;
    char address[BT_ADDR_MAX] = "";
    char action[16] = "";
    http_form_get_param(req, "address=", address, sizeof(address));
    http_form_get_param(req, "action=", action, sizeof(action));

    if (strlen(address) != 17) { http_err(fd, 400, "Bad Request", "invalid device address"); return; }
    if (!action[0]) { http_err(fd, 400, "Bad Request", "missing action"); return; }

    /* 立即返回的动作：删除/信任只改本地配对�?*/
    if (!strcmp(action, "remove") || !strcmp(action, "trust") || !strcmp(action, "untrust")) {
        int ok = bt_device_action(address, action) == 0;
        last_set(action, address, ok, ok ? "" : bt_last_error());
        char esc[600], json[800];
        http_json_escape(ok ? "" : bt_last_error(), esc, sizeof(esc));
        int n = snprintf(json, sizeof(json), "{\"ok\":%s,\"error\":\"%s\"}", ok ? "true" : "false", esc);
        http_ok_json(fd, json, (size_t)n);
        return;
    }

    if (strcmp(action, "pair") && strcmp(action, "connect") && strcmp(action, "disconnect")) {
        http_err(fd, 400, "Bad Request", "unknown action");
        return;
    }

    bt_job_t *job = calloc(1, sizeof(*job));
    if (!job) { http_err(fd, 500, "Error", "out of memory"); return; }
    snprintf(job->address, sizeof(job->address), "%s", address);
    snprintf(job->action, sizeof(job->action), "%s", action);

    if (http_worker_start(bt_job_worker, job) != 0) {
        free(job);
        http_err(fd, 503, "Service Unavailable", "{\"ok\":false,\"error\":\"cannot start worker\"}");
        return;
    }

    static const char accepted[] = "{\"ok\":true,\"accepted\":true}";
    http_ok_json(fd, accepted, sizeof(accepted) - 1);
}
