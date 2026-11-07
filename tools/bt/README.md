# 板载蓝牙（AIC8800D80 → BlueZ）

ROCK 4D 上的 WiFi/蓝牙是同一颗 **AICSemi AIC8800D80**（USB，`1a40:0101` 之后的 `a69c:8d81`）。
厂商内核驱动 `aic_btusb` 默认编成 **Android bluedroid 模式**：只向用户态暴露
`/dev/aicbt_dev` 字符设备，不注册 hci 设备，因此标准 BlueZ 完全看不到这块蓝牙。

本目录提供一个最小改动方案：**同一份驱动源码用 `CONFIG_BLUEDROID=0` 重编**，
它就会走标准 Linux HCI 分支，注册 `hci0`，BlueZ/`bluetoothctl` 直接可用。

## 1. 编译并安装

```bash
sudo ./build-aic-bt-bluez.sh
```

脚本做四件事：

1. 从内核 bundle 复制 `aic_btusb` 源码到临时目录（路径可用 `AICBT_SRC` 覆盖，
   默认 `~/kbuild/kernels/<版本>/preparation/bundle/aic-bluetooth/aic_btusb`）；
2. 把 `aic_btusb.h` 里的 `CONFIG_BLUEDROID` 改成 `0`，用当前内核的 `build` 树重编；
3. 装到 `/lib/modules/$(uname -r)/updates/aic_btusb_bluez.ko`，并写
   `/etc/modprobe.d/aicbt-bluez.conf`（`install aic_btusb …insmod` 覆盖默认模块）
   与 `/etc/modules-load.d/aicbt-bluez.conf`（开机加载）；
4. `depmod -a` 并启用 `bluetooth.service`。

验证：

```bash
ls /sys/class/bluetooth          # 应该出现 hci0
bluetoothctl show                # Controller E4:85:FB:… Powered: yes
```

> 驱动源码不在本仓库：它随内核 bundle 发布（`rock-4d-npu-kernel`）。
> 内核升级后需要在内核更新过的树上重跑一次脚本。

## 2. 运行时依赖

```bash
sudo apt install -y bluez
sudo systemctl enable --now bluetooth
```

## 3. Web 接口

配置页「蓝牙连接」卡片对应下面这组接口（`source/http/http_api_bluetooth.c`，
底层是 `source/bluetooth/bt_manager.c`）：

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/api/bluetooth/status` | — | 适配器状态 + 最近一次动作结果 |
| GET | `/api/bluetooth/devices` | — | 已发现/已配对设备（按连接→配对→信号排序） |
| POST | `/api/bluetooth/adapter` | `powered=on\|off`、`discoverable=on\|off`、`pairable=on\|off`、`name=` | 适配器属性 |
| POST | `/api/bluetooth/scan` | `on=on\|off` | 开始/停止扫描 |
| POST | `/api/bluetooth/device` | `address=..&action=pair\|connect\|disconnect\|trust\|untrust\|remove` | 设备动作 |

实现要点：

- 状态查询走 **sd-bus**（libsystemd 已在链接），一次 `GetManagedObjects` 拿对象
  列表，属性逐项 `sd_bus_get_property`——手写 `a{sv}` 解析极易容器错位。
- 配对/连接走 **`bluetoothctl` 子进程**（它自带配对 agent），30 秒级的阻塞动作
  交给 worker 线程；接口立刻返回 `{"ok":true,"accepted":true}`，前端轮询
  `status.last` 取结果。
- 成功判定不看 `bluetoothctl` 的文案，而是动作后回查 BlueZ 的
  `Paired` / `Connected` / `Trusted` 属性，避免"提示成功但其实没配上"。

## 4. 已知限制

- 适配器不支持 `Connectable`（AIC 固件上报 false），主动连某些 BR/EDR 设备可能失败；
  BLE 传感器/耳机这类 JustWorks 设备正常。
- 扫描期间 `bluetoothd` 会持续占用 HCI；本工程只在用户点「扫描」时开启。
- 蓝牙与 WiFi 共用一颗芯片，2.4G 频段高负载时相互影响（固件 coexistence 官方即如此）。

## 2.5 蓝牙音频端点（A2DP / HFP）

AirPods、蓝牙耳机/音箱属于 BR/EDR 音频设备：配对完成后 BlueZ 必须立刻建立
A2DP（或 HFP）连接。系统里若没有音频守护进程向 BlueZ 注册 `MediaEndpoint`，
`bluetoothd` 会报

```
btd_service_connect() a2dp-sink profile connect failed ... Protocol not available
```

对端随即断开，bond 来不及落盘，表现就是"配对不上"（`bluetoothctl pair` 当下会
打印 `Pairing successful`，但设备很快从缓存消失、`/var/lib/bluetooth/<adapter>/`
下没有设备目录）。

安装（无桌面会话也能用，PulseAudio 系统模式）：

```bash
sudo ./install-bt-audio.sh
```

它会装 `pulseaudio` + `pulseaudio-module-bluetooth`，在 `/etc/pulse/system.pa` 里
启用 `module-bluetooth-discover`，并以 `pulseaudio-system.service` 常驻。成功后
`bluetoothctl show` 的 UUID 里会出现：

```
Audio Source (0000110a)  Audio Sink (0000110b)
Headset (00001108)       Handsfree Audio Gateway (0000111f)
```

验证：

```bash
bluetoothctl show | grep -iE "Audio Sink|Audio Source"
journalctl -u bluetooth | grep -i "Endpoint registered"
```

> 之前尝试过 PipeWire + WirePlumber（`libspa-0.2-bluetooth`）：WirePlumber 的
> `monitor.bluez` 脚本能加载，但始终没有向 BlueZ 注册端点，`busctl tree org.bluez`
> 里看不到 `/MediaEndpoint/*`，因此改用 PulseAudio 系统模式。