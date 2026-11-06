# 自动驾驶小车（Radxa ROCK 4D / RK3576）

一台基于 RK3576 的视觉循迹小车：单进程 C 服务同时负责 **摄像头采集 → NPU 推理（YOLO）→ 目标跟踪 → CAN 电机输出**，
并内置一个轻量 HTTP 服务提供网页控制台。整机跑在 Armbian（Debian 13 trixie）上，桌面、手机浏览器都能直接操作。

---

## 硬件

| 项目 | 规格 |
|---|---|
| 主板 | Radxa ROCK 4D（SoC：RK3576，8 核 4×A72 + 4×A53） |
| 内存 / 存储 | 6 GB LPDDR5 / 476 GB NVMe |
| 内核 | `7.4.0-rc1-npu-rt-full`（PREEMPT_RT + RKNPU 驱动，自行编译） |
| 摄像头 | UVC（默认 `/dev/video2`，1280×720@30） |
| 执行器 | CAN FD 电机驱动（默认未配置，见 `config/car_motor_can.example.conf`） |
| 加速器 | NPU `/dev/dri/renderD128`、GPU `renderD129`、RGA `/dev/rga` |

## 软件结构

```
source/
  core/        配置读写、日志
  http/        HTTP 服务与各 API（config/system/can/video/yolo/tracking/rec/...）
  video/       V4L2 采集、MPP 编码、MJPEG 推流
  yolo/        RKNN 推理池（多线程 + RGA 预处理）
  control/     跟随控制器（PID、安全门限）
  can/         CAN FD 收发与电机协议映射
  telemetry/   匿名助手（ANO）链路
include/       对应头文件
html/          前端（纯静态，无构建步骤）
scripts/       build.sh / deploy.sh / model_sync.sh / proxy_config.py / codex_bridge.py / git_sync.sh
config/        运行期配置（随部署同步，可被网页修改）
```

服务单元：`autonomous-driving-car.service`（`/opt/autonomous-driving-car`，root 运行，带 systemd 沙箱：
`ProtectSystem=strict`、`ProtectHome=read-only`、`PrivateTmp=yes`，只放开 config/recordings/.codex 可写）。

## 构建与部署

```bash
./scripts/build.sh -R          # Release 构建，产物 bin/autonomous-driving-car
sudo ./scripts/deploy.sh -i    # 安装到 /opt/autonomous-driving-car 并重启服务
sudo ./scripts/deploy.sh -u    # 卸载
```

依赖：`librga.so`（RGA 预处理）、RKNN 运行库、MPP；`scripts/build.sh` 会先做静态检查与模型同步。

## 网页控制台

| 路径 | 作用 |
|---|---|
| `/` | 主界面：实时画面（MJPEG）、YOLO 检测框叠加、目标追踪、Codex 对话 |
| `/status` | 系统状态：CPU（逐核 + 频率）、内存/磁盘、NPU/RGA 负载、网络速率 |
| `/config` | 系统配置：YOLO、CPU 频率、Web/ANO、视频采集、网络代理、智能跟随、Codex CLI |
| `/logs` | 文件下载：行驶日志、录像、检测结果、Codex 交互记录 |
| `/dbc` | CAN DBC 解析与实时报文 |

前端无框架，全部静态文件；样式基于主题变量（深/浅色可切换）。所有写操作走 `authFetch`（Basic Auth +
自绘管理员登录框），只读接口用浏览器自带凭据。

## 主要 HTTP 接口

```
GET  /api/system             CPU/内存/磁盘/温度/NPU/RGA
GET  /api/config             运行配置
POST /api/config             保存配置
GET  /api/video/devices|caps 摄像头枚举
GET  /api/video/stats        采集/推理统计
GET  /api/follow            跟随控制器状态
POST /api/follow             修改跟随参数
GET  /api/can/ifaces         CAN 接口
POST /api/can/toggle         开关 CAN
GET  /api/proxy/config       网络代理配置（含 enabled 开关）
POST /api/proxy/config       保存/应用代理
GET  /api/nettest            Ping / 网速测试
GET  /api/codex/status       Codex 登录与运行状态
GET  /api/codex/models       模型目录（来自 codex debug models，缓存 1 小时）
GET  /api/codex/quota        额度（来自 codex app-server，缓存 60 秒）
POST /api/codex/chat         发起一轮对话（后台执行）
POST /api/codex/update/install  开始后台升级 Codex CLI
GET  /api/codex/update/status   升级进度（下载字节数/阶段）
```

## 配置文件

| 文件 | 说明 |
|---|---|
| `config/config.txt` | 主配置（视频、YOLO、CAN、网卡） |
| `config/follow.conf` | 跟随控制器 PID 与限幅参数 |
| `config/car_motor_can.conf` | 电机 CAN 协议映射（未配置时电机不输出） |
| `config/cpu_freq.conf` | CPU 调频策略 |
| `/etc/proxy-client.conf` | 整机网络代理（由网页「网络代理」卡片管理，开关即启停 `proxy-client.service`） |

## 模型

`config/yolo26.rknn` 为 RK3576 平台模型，`scripts/model_sync.sh` 负责在仓库与部署目录之间做一致性校验，
网页支持上传替换（`.rknn` + `.names`）。

## Codex CLI 集成

`scripts/codex_bridge.py` 是 HTTP 层与 Codex CLI 之间的桥：每次调用短生命周期、独立进程。
- 登录：`codex login --device-auth`（PTY，抓取设备码与链接）
- 对话：`codex exec --json`，prompt 走 stdin，结果写入 `recordings/codex/`
- 额度/模型：`codex app-server`（JSON-RPC）与 `codex debug models`
- 升级：`systemd-run` 起独立单元在宿主命名空间下载安装（服务沙箱内无法写 `~/.codex`），
  进度写入 `recordings/codex/codex_update_progress.txt`，网页每 2.5 秒轮询

交互记录为人类可读纯文本：

```
23:18:38 你：你好
23:19:04 Codex：……
23:19:20 错误：……
```

## 开发约定

- 改完代码先 `./scripts/build.sh -R`，再 `sudo ./scripts/deploy.sh -i`
- 部署后同步仓库：`./scripts/git_sync.sh "一句话说明"`（提交 + 推送 origin/main）
- 前端静态文件带 `Cache-Control: no-cache`，部署后刷新即生效；不要改成 `no-store`（会让每次切页全量重下）
- 大块样式改动请先在浏览器里确认渲染，再提交

## 许可

GPL-3.0-or-later，见 `LICENSE`。
