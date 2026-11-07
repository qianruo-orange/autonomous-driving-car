/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef BT_MANAGER_H
#define BT_MANAGER_H

#include <stddef.h>

/**
 * bt_manager.h — 板载蓝牙（BlueZ）适配器/设备管理。
 *
 * 查询走 D-Bus（org.bluez + ObjectManager），一次 GetManagedObjects 拿到
 * 适配器和全部设备的属性；配对/连接这类会阻塞几十秒的动作交给 bluetoothctl
 * 子进程（它自带配对 agent），由 HTTP 层放进 worker 线程，避免卡住反应堆。
 */

#define BT_ADDR_MAX     24
#define BT_PATH_MAX    192
#define BT_NAME_MAX    128
#define BT_MAX_DEVICES  64

typedef struct {
    char path[BT_PATH_MAX];
    char address[BT_ADDR_MAX];
    char name[BT_NAME_MAX];
    char alias[BT_NAME_MAX];
    char icon[64];
    int  paired;
    int  connected;
    int  trusted;
    int  has_rssi;
    int  rssi;
    unsigned int dev_class;
} bt_device_t;

typedef struct {
    int  available;      /* BlueZ 可用且存在适配器 */
    int  powered;
    int  discoverable;
    int  pairable;
    int  discovering;
    char path[BT_PATH_MAX];
    char address[BT_ADDR_MAX];
    char name[BT_NAME_MAX];
    char alias[BT_NAME_MAX];
    int  device_count;
} bt_adapter_t;

/* 读取适配器与设备快照。成功返回 0；D-Bus/BlueZ 不可用返回 -1。 */
int bt_snapshot(bt_adapter_t *adapter, bt_device_t *devices, int max_devices);

/* 适配器属性。成功返回 0，失败返回 -1（错误信息见 bt_last_error）。 */
int bt_set_powered(int on);
int bt_set_discoverable(int on);
int bt_set_pairable(int on);
int bt_set_name(const char *name);
int bt_set_discovering(int on);

/* action: "pair" / "connect" / "disconnect" / "trust" / "untrust" / "remove"
 * 阻塞时间较长（配对可达 30s），不要在 HTTP 反应堆线程直接调用。 */
int bt_device_action(const char *address, const char *action);

const char *bt_last_error(void);

#endif /* BT_MANAGER_H */
