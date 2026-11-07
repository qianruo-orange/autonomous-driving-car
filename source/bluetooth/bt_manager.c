/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * bt_manager.c — 通过 BlueZ 的 D-Bus 接口管理板载蓝牙。
 *
 * 设计要点：
 *  - 只保留一条到 system bus 的连接（g_bus），所有访问都在 g_lock 下串行；
 *    sd-bus 连接不是线程安全的，而 HTTP 反应堆与 worker 线程会同时用到它。
 *  - 枚举对象只用 ObjectManager（扁平 skip 遍历，只取 path 与接口名），
 *    单个属性一律走 sd_bus_get_property：variant 由 sd-bus 内部拆包，
 *    避免手写 a{sv} 解析时容器进出错位（那会让后续对象全部丢失）。
 *  - 配对/连接用 bluetoothctl 子进程：BlueZ 需要配对 agent，bluetoothctl
 *    自带，且只在用户点击时启动，代价可接受。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include "bluetooth/bt_manager.h"
#include "core/log.h"

#define BLUEZ_SERVICE   "org.bluez"
#define ADAPTER_IFACE   "org.bluez.Adapter1"
#define DEVICE_IFACE    "org.bluez.Device1"
#define PROPS_IFACE     "org.freedesktop.DBus.Properties"
#define OBJMGR_IFACE    "org.freedesktop.DBus.ObjectManager"
#define CALL_TIMEOUT_US (5 * 1000 * 1000)
#define BT_MAX_OBJECTS  128

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sd_bus *g_bus;
static char g_err[256];

static void set_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

const char *bt_last_error(void) { return g_err; }

static int bus_get(void)
{
    if (g_bus) return 0;
    int r = sd_bus_open_system(&g_bus);
    if (r < 0) {
        set_err("cannot connect to system bus: %s", strerror(-r));
        g_bus = NULL;
        return -1;
    }
    (void)sd_bus_set_method_call_timeout(g_bus, CALL_TIMEOUT_US);
    return 0;
}

/* ---- 属性读取（variant 交给 sd-bus 处理） ---- */

static int prop_str(sd_bus *bus, const char *path, const char *iface,
                    const char *name, char *out, size_t size)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int r = sd_bus_get_property(bus, BLUEZ_SERVICE, path, iface, name, &err, &m, "s");
    if (r >= 0) {
        const char *s = NULL;
        if (sd_bus_message_read(m, "s", &s) >= 0 && s) snprintf(out, size, "%s", s);
    }
    if (m) sd_bus_message_unref(m);
    sd_bus_error_free(&err);
    return r < 0 ? -1 : 0;
}

static int prop_bool(sd_bus *bus, const char *path, const char *iface, const char *name)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int v = 0;
    int r = sd_bus_get_property(bus, BLUEZ_SERVICE, path, iface, name, &err, &m, "b");
    if (r >= 0) (void)sd_bus_message_read(m, "b", &v);
    if (m) sd_bus_message_unref(m);
    sd_bus_error_free(&err);
    return r < 0 ? 0 : v;
}

static int prop_int16(sd_bus *bus, const char *path, const char *iface,
                      const char *name, int *out)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int16_t v = 0;
    int r = sd_bus_get_property(bus, BLUEZ_SERVICE, path, iface, name, &err, &m, "n");
    if (r >= 0 && sd_bus_message_read(m, "n", &v) >= 0) { *out = v; r = 0; }
    else r = -1;
    if (m) sd_bus_message_unref(m);
    sd_bus_error_free(&err);
    return r;
}

static int prop_uint32(sd_bus *bus, const char *path, const char *iface,
                       const char *name, unsigned int *out)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    uint32_t v = 0;
    int r = sd_bus_get_property(bus, BLUEZ_SERVICE, path, iface, name, &err, &m, "u");
    if (r >= 0 && sd_bus_message_read(m, "u", &v) >= 0) { *out = v; r = 0; }
    else r = -1;
    if (m) sd_bus_message_unref(m);
    sd_bus_error_free(&err);
    return r;
}

/* ---- 对象枚举 ---- */

typedef struct {
    char path[BT_PATH_MAX];
    int  adapter;
    int  device;
} bt_obj_t;

/* 扁平遍历 GetManagedObjects：只读 path 与接口名，属性整块 skip。
 * 这条路径在板子上验证过不会错位（属性解析走 get_property 单独取）。 */
static int list_objects(sd_bus *bus, bt_obj_t *objs, int max, int *count)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;

    *count = 0;
    int r = sd_bus_call_method(bus, BLUEZ_SERVICE, "/", OBJMGR_IFACE,
                               "GetManagedObjects", &err, &m, "");
    if (r < 0) {
        set_err("GetManagedObjects: %s", err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return -1;
    }
    sd_bus_error_free(&err);

    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{oa{sa{sv}}}") <= 0) {
        set_err("unexpected GetManagedObjects payload");
        sd_bus_message_unref(m);
        return -1;
    }

    while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "oa{sa{sv}}") > 0) {
        const char *opath = NULL;
        if (sd_bus_message_read_basic(m, 'o', &opath) <= 0) break;

        int idx = -1;
        if (opath && *count < max) {
            idx = *count;
            memset(&objs[idx], 0, sizeof(objs[idx]));
            snprintf(objs[idx].path, sizeof(objs[idx].path), "%s", opath);
        }

        if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sa{sv}}") > 0) {
            while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sa{sv}") > 0) {
                const char *iface = NULL;
                (void)sd_bus_message_read_basic(m, 's', &iface);
                if (idx >= 0 && iface) {
                    if (!strcmp(iface, ADAPTER_IFACE)) objs[idx].adapter = 1;
                    else if (!strcmp(iface, DEVICE_IFACE)) objs[idx].device = 1;
                }
                (void)sd_bus_message_skip(m, "a{sv}");
                sd_bus_message_exit_container(m);
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);

        if (idx >= 0 && (objs[idx].adapter || objs[idx].device)) (*count)++;
    }

    sd_bus_message_unref(m);
    return 0;
}

static int find_adapter_locked(bt_obj_t *objs, int n, bt_obj_t *out)
{
    for (int i = 0; i < n; i++) {
        if (objs[i].adapter) { *out = objs[i]; return 0; }
    }
    set_err("no Bluetooth adapter");
    return -1;
}

static void fill_adapter(sd_bus *bus, const bt_obj_t *o, bt_adapter_t *a)
{
    memset(a, 0, sizeof(*a));
    a->available = 1;
    snprintf(a->path, sizeof(a->path), "%s", o->path);
    (void)prop_str(bus, o->path, ADAPTER_IFACE, "Address", a->address, sizeof(a->address));
    (void)prop_str(bus, o->path, ADAPTER_IFACE, "Name", a->name, sizeof(a->name));
    (void)prop_str(bus, o->path, ADAPTER_IFACE, "Alias", a->alias, sizeof(a->alias));
    if (!a->alias[0]) snprintf(a->alias, sizeof(a->alias), "%s", a->name);
    a->powered = prop_bool(bus, o->path, ADAPTER_IFACE, "Powered");
    a->discoverable = prop_bool(bus, o->path, ADAPTER_IFACE, "Discoverable");
    a->pairable = prop_bool(bus, o->path, ADAPTER_IFACE, "Pairable");
    a->discovering = prop_bool(bus, o->path, ADAPTER_IFACE, "Discovering");
}

static void fill_device(sd_bus *bus, const bt_obj_t *o, bt_device_t *d)
{
    memset(d, 0, sizeof(*d));
    snprintf(d->path, sizeof(d->path), "%s", o->path);
    (void)prop_str(bus, o->path, DEVICE_IFACE, "Address", d->address, sizeof(d->address));
    (void)prop_str(bus, o->path, DEVICE_IFACE, "Name", d->name, sizeof(d->name));
    (void)prop_str(bus, o->path, DEVICE_IFACE, "Alias", d->alias, sizeof(d->alias));
    (void)prop_str(bus, o->path, DEVICE_IFACE, "Icon", d->icon, sizeof(d->icon));
    if (!d->alias[0]) snprintf(d->alias, sizeof(d->alias), "%s", d->name);
    d->paired = prop_bool(bus, o->path, DEVICE_IFACE, "Paired");
    d->connected = prop_bool(bus, o->path, DEVICE_IFACE, "Connected");
    d->trusted = prop_bool(bus, o->path, DEVICE_IFACE, "Trusted");
    d->has_rssi = prop_int16(bus, o->path, DEVICE_IFACE, "RSSI", &d->rssi) == 0;
    (void)prop_uint32(bus, o->path, DEVICE_IFACE, "Class", &d->dev_class);
}

int bt_snapshot(bt_adapter_t *adapter, bt_device_t *devices, int max_devices)
{
    bt_obj_t objs[BT_MAX_OBJECTS];
    int n = 0, r;
    pthread_mutex_lock(&g_lock);
    if (bus_get() < 0) { pthread_mutex_unlock(&g_lock); return -1; }
    r = list_objects(g_bus, objs, BT_MAX_OBJECTS, &n);
    if (r < 0) { pthread_mutex_unlock(&g_lock); return -1; }

    bt_obj_t aobj;
    int have_adapter = find_adapter_locked(objs, n, &aobj) == 0;
    if (adapter) {
        if (have_adapter) fill_adapter(g_bus, &aobj, adapter);
        else { memset(adapter, 0, sizeof(*adapter)); if (!g_err[0]) set_err("no Bluetooth adapter"); }
    }

    int used = 0;
    if (devices) {
        for (int i = 0; i < n && used < max_devices; i++) {
            if (!objs[i].device) continue;
            fill_device(g_bus, &objs[i], &devices[used]);
            if (devices[used].address[0]) used++;
        }
    }
    if (adapter) adapter->device_count = used;
    pthread_mutex_unlock(&g_lock);
    return have_adapter ? 0 : -1;
}

/* ---- 适配器属性写入 ---- */

static int adapter_path_locked(char *path, size_t size)
{
    bt_obj_t objs[BT_MAX_OBJECTS], aobj;
    int n = 0;
    if (list_objects(g_bus, objs, BT_MAX_OBJECTS, &n) < 0) return -1;
    if (find_adapter_locked(objs, n, &aobj) < 0) return -1;
    snprintf(path, size, "%s", aobj.path);
    return 0;
}

static int set_prop_bool(const char *iface, const char *name, int on)
{
    int r = -1;
    pthread_mutex_lock(&g_lock);
    if (bus_get() < 0) { pthread_mutex_unlock(&g_lock); return -1; }

    char path[BT_PATH_MAX];
    if (adapter_path_locked(path, sizeof(path)) < 0) { pthread_mutex_unlock(&g_lock); return -1; }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    r = sd_bus_message_new_method_call(g_bus, &m, BLUEZ_SERVICE, path, PROPS_IFACE, "Set");
    if (r < 0) { set_err("new_method_call: %s", strerror(-r)); goto out; }

    r = sd_bus_message_append(m, "ss", iface, name);
    if (r < 0) goto out;
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "b");
    if (r < 0) goto out;
    r = sd_bus_message_append(m, "b", (int)(on != 0));
    if (r < 0) goto out;
    r = sd_bus_message_close_container(m);
    if (r < 0) goto out;

    r = sd_bus_call(g_bus, m, CALL_TIMEOUT_US, &err, NULL);
    if (r < 0) set_err("set %s: %s", name, err.message ? err.message : strerror(-r));
out:
    sd_bus_error_free(&err);
    if (m) sd_bus_message_unref(m);
    pthread_mutex_unlock(&g_lock);
    return r < 0 ? -1 : 0;
}

int bt_set_powered(int on)      { return set_prop_bool(ADAPTER_IFACE, "Powered", on); }
int bt_set_discoverable(int on) { return set_prop_bool(ADAPTER_IFACE, "Discoverable", on); }
int bt_set_pairable(int on)     { return set_prop_bool(ADAPTER_IFACE, "Pairable", on); }

int bt_set_name(const char *name)
{
    int r = -1;
    if (!name || !*name || strlen(name) > 63) { set_err("invalid adapter name"); return -1; }
    pthread_mutex_lock(&g_lock);
    if (bus_get() < 0) { pthread_mutex_unlock(&g_lock); return -1; }
    char path[BT_PATH_MAX];
    if (adapter_path_locked(path, sizeof(path)) < 0) { pthread_mutex_unlock(&g_lock); return -1; }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    r = sd_bus_message_new_method_call(g_bus, &m, BLUEZ_SERVICE, path, PROPS_IFACE, "Set");
    if (r < 0) goto out;
    r = sd_bus_message_append(m, "ss", ADAPTER_IFACE, "Alias");
    if (r < 0) goto out;
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "s");
    if (r < 0) goto out;
    r = sd_bus_message_append(m, "s", name);
    if (r < 0) goto out;
    r = sd_bus_message_close_container(m);
    if (r < 0) goto out;
    r = sd_bus_call(g_bus, m, CALL_TIMEOUT_US, &err, NULL);
    if (r < 0) set_err("set Alias: %s", err.message ? err.message : strerror(-r));
out:
    sd_bus_error_free(&err);
    if (m) sd_bus_message_unref(m);
    pthread_mutex_unlock(&g_lock);
    return r < 0 ? -1 : 0;
}

int bt_set_discovering(int on)
{
    int r = -1;
    pthread_mutex_lock(&g_lock);
    if (bus_get() < 0) { pthread_mutex_unlock(&g_lock); return -1; }
    char path[BT_PATH_MAX];
    if (adapter_path_locked(path, sizeof(path)) < 0) { pthread_mutex_unlock(&g_lock); return -1; }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(g_bus, BLUEZ_SERVICE, path, ADAPTER_IFACE,
                           on ? "StartDiscovery" : "StopDiscovery", &err, NULL, "");
    if (r < 0) {
        const char *msg = err.message ? err.message : "";
        if (strstr(msg, "InProgress") || strstr(msg, "Operation already") ||
            strstr(msg, "NotReady") || strstr(msg, "No discovery"))
            r = 0;
        else
            set_err("%s discovery: %s", on ? "start" : "stop", msg[0] ? msg : strerror(-r));
    }
    sd_bus_error_free(&err);
    pthread_mutex_unlock(&g_lock);
    return r < 0 ? -1 : 0;
}

/* ---- 设备动作（bluetoothctl 子进程，自带配对 agent） ---- */

static int run_bluetoothctl(const char *const *args, char *out, size_t out_size)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) { set_err("pipe: %s", strerror(errno)); return -1; }

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); set_err("fork: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp("bluetoothctl", (char *const *)args);
        _exit(127);
    }

    close(pipefd[1]);
    size_t used = 0;
    if (out && out_size) {
        ssize_t n;
        while ((n = read(pipefd[0], out + used, out_size - 1 - used)) > 0) {
            used += (size_t)n;
            if (used >= out_size - 1) break;
        }
        out[used] = 0;
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { set_err("waitpid: %s", strerror(errno)); return -1; }
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* 读取某地址设备的真实状态。present=1 表示设备仍在 BlueZ 里。
 * 返回 0 成功（含 present=0），-1 表示 D-Bus 不可用。 */
static int device_state(const char *address, int *present, int *paired,
                        int *connected, int *trusted)
{
    bt_obj_t objs[BT_MAX_OBJECTS];
    int n = 0, found = 0;

    *present = 0; *paired = 0; *connected = 0; *trusted = 0;

    pthread_mutex_lock(&g_lock);
    if (bus_get() < 0) { pthread_mutex_unlock(&g_lock); return -1; }
    if (list_objects(g_bus, objs, BT_MAX_OBJECTS, &n) == 0) {
        for (int i = 0; i < n && !found; i++) {
            if (!objs[i].device) continue;
            char addr[BT_ADDR_MAX] = "";
            if (prop_str(g_bus, objs[i].path, DEVICE_IFACE, "Address", addr, sizeof(addr)) < 0) continue;
            if (strcasecmp(addr, address) != 0) continue;
            found = 1;
            *present = 1;
            *paired = prop_bool(g_bus, objs[i].path, DEVICE_IFACE, "Paired");
            *connected = prop_bool(g_bus, objs[i].path, DEVICE_IFACE, "Connected");
            *trusted = prop_bool(g_bus, objs[i].path, DEVICE_IFACE, "Trusted");
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int bt_device_action(const char *address, const char *action)
{
    if (!address || strlen(address) != 17) { set_err("invalid device address"); return -1; }
    if (!action) { set_err("missing action"); return -1; }

    /* 先看 BlueZ 里的真实状态：已满足的动作直接返回，未发现的设备快速失败。
     * 否则对已配对设备再执行 pair 会一直等到 bluetoothctl 超时（35s），
     * 前端只能干等。 */
    int present = 0, paired = 0, connected = 0, trusted = 0;
    if (device_state(address, &present, &paired, &connected, &trusted) == 0) {
        if (!strcmp(action, "pair")) {
            if (paired) return 0;
        } else if (!strcmp(action, "connect")) {
            if (connected) return 0;
        } else if (!strcmp(action, "disconnect")) {
            if (!connected) return 0;
        } else if (!strcmp(action, "trust")) {
            if (trusted) return 0;
        } else if (!strcmp(action, "untrust")) {
            if (!trusted) return 0;
        } else if (!strcmp(action, "remove")) {
            if (!present) return 0;
        }
        if (!present) {
            set_err("%s: device %s not in BlueZ cache — scan first", action, address);
            return -1;
        }
    }

    char out[2048] = "";
    int rc;

    if (!strcmp(action, "remove")) {
        const char *args[] = { "bluetoothctl", "remove", address, NULL };
        rc = run_bluetoothctl(args, out, sizeof(out));
    } else if (!strcmp(action, "trust") || !strcmp(action, "untrust")) {
        const char *args[] = { "bluetoothctl", !strcmp(action, "trust") ? "trust" : "untrust", address, NULL };
        rc = run_bluetoothctl(args, out, sizeof(out));
    } else if (!strcmp(action, "pair") || !strcmp(action, "connect") ||
               !strcmp(action, "disconnect")) {
        const char *args[] = { "bluetoothctl", "--timeout", "20", action, address, NULL };
        rc = run_bluetoothctl(args, out, sizeof(out));
    } else {
        set_err("unknown action '%s'", action);
        return -1;
    }

    /* bluetoothctl 的退出码和文案都不足以判定结果（"Pairing successful" 之后
     * 仍可能因对端拒绝而没有真正配对），这里以 BlueZ 的实际属性为准。 */
    int state_ok = device_state(address, &present, &paired, &connected, &trusted) == 0;

    int ok;
    if (!strcmp(action, "pair"))            ok = state_ok && present && paired;
    else if (!strcmp(action, "connect"))    ok = state_ok && present && connected;
    else if (!strcmp(action, "disconnect")) ok = state_ok && present && !connected;
    else if (!strcmp(action, "trust"))      ok = state_ok && present && trusted;
    else if (!strcmp(action, "untrust"))    ok = state_ok && present && !trusted;
    else /* remove */                       ok = state_ok && !present;

    if (!ok) {
        char *nl = strpbrk(out, "\r\n");
        if (nl) *nl = 0;
        if (state_ok) {
            set_err("%s %s failed (BlueZ: present=%d paired=%d connected=%d)%s%s",
                    action, address, present, paired, connected,
                    out[0] ? "; " : "", out);
        } else {
            set_err("%s %s: %s", action, address,
                    out[0] ? out : (rc == 127 ? "bluetoothctl not found" : "failed"));
        }
        LOG_ERROR("bluetooth: %s", g_err);
        return -1;
    }
    return 0;
}
