# 自动驾驶小车（Radxa ROCK 4D / RK3576）

一台基于 RK3576 的视觉循迹小车：单进程 C 服务同时负责 **摄像头采集 → NPU 推理（YOLO）→ 目标跟踪 → CAN 电机输出**，
并内置轻量 HTTP 服务提供网页控制台。桌面浏览器、手机都能直接操作。

---

## 硬件

| 项目 | 规格 |
|---|---|
| 主板 | Radxa ROCK 4D（RK3576，8 核 4×A72 + 4×A53） |
| 内存 / 存储 | 6 GB LPDDR5 / 476 GB NVMe |
| 内核 | `7.4.0-rc1-npu-rt-full`（PREEMPT_RT + RKNPU，自行编译） |
| 摄像头 | UVC（默认 `/dev/video2`，1280×720@30） |
| 执行器 | CAN FD 电机（默认未配置，见 `config/car_motor_can.example.conf`） |
| 加速器 | NPU `/dev/dri/renderD128`、GPU `renderD129`、RGA `/dev/rga` |

## 代码结构

```
source/
  core/        配置读写、日志
  http/        HTTP 服务与 API（config/system/can/video/yolo/tracking/rec/...）
  video/       V4L2 采集、MPP 编码、MJPEG 推流
  yolo/        RKNN 推理池（多线程 + RGA 预处理）
  control/     跟随控制器（PID、安全门限）
  can/         CAN FD 收发与电机协议映射
  telemetry/   匿名助手（ANO）链路
html/          前端（纯静态，无构建步骤）
scripts/       build.sh / deploy.sh / model_sync.sh / proxy_config.py / codex_bridge.py / git_sync.sh
config/        运行期配置（随部署同步，可被网页修改）
```

服务单元 `autonomous-driving-car.service` 运行于 `/opt/autonomous-driving-car`，带 systemd 沙箱：
`ProtectSystem=strict`、`ProtectHome=read-only`、`PrivateTmp=yes`，仅放开 config / recordings / .codex 可写。
（因此 Codex 升级必须用 `systemd-run` 逃出沙箱，见下文。）

## 构建与部署

```bash
./scripts/build.sh -R          # Release 构建 → bin/autonomous-driving-car
sudo ./scripts/deploy.sh -i    # 安装到 /opt/autonomous-driving-car 并重启
sudo ./scripts/deploy.sh -u    # 卸载
./scripts/git_sync.sh "说明"    # 提交并推送 origin/main
```

依赖：`librga.so`、RKNN 运行库、MPP。`build.sh` 会先做依赖与模型同步检查。

## 网页控制台

| 路径 | 作用 |
|---|---|
| `/` | 主界面：MJPEG 实时画面、检测框叠加、目标追踪、Codex 对话、顶栏时钟 |
| `/status` | 系统状态：CPU（逐核 + 温度）、内存/磁盘、NPU/RGA、网络速率 |
| `/config` | 系统配置：YOLO、CPU 频率、网络代理、Web/ANO、视频采集、智能跟随、Codex CLI |
| `/logs` | 文件下载：日志、录像、检测结果、Codex 交互记录 |
| `/dbc` | CAN DBC 解析与实时报文 |

### 静态资源与缓存

- 所有 CSS/JS 引用带版本号：`/css/config.css?v=2026110701`
- 服务端在 `http_serve_file()` 入口剥掉 `?query` 再找文件（`source/http/http.c`）
- 响应头为 `Cache-Control: no-cache`；**不要**改成 `no-store`（会让每次切页全量重下，明显变卡）
- 部署后要立刻生效：改 `finish_two.sh` 里的 `VER` 重新生成版本号

## 主要 HTTP 接口

```
GET  /api/system                  CPU/内存/磁盘/温度/NPU/RGA
GET  /api/config                  运行配置          POST 保存
GET  /api/video/devices|caps      摄像头枚举
GET  /api/video/stats             采集/推理统计
GET  /api/follow                  跟随控制器状态    POST 参数
GET  /api/can/ifaces              CAN 接口
GET  /api/proxy/config            代理配置（含 enabled 开关）
POST /api/proxy/config            保存/应用代理（启停 proxy-client.service）
GET  /api/nettest                 Ping / 网速测试
GET  /api/codex/status            Codex 登录与运行状态
GET  /api/codex/models            模型目录（codex debug models，缓存 1 小时）
GET  /api/codex/quota             额度（app-server，缓存 60 秒）
POST /api/codex/chat              发起一轮对话（后台执行）
POST /api/codex/update/install    开始后台升级
GET  /api/codex/update/status     升级进度（已下载字节/阶段/版本）
```

## 配置文件

| 文件 | 说明 |
|---|---|
| `config/config.txt` | 主配置（视频、YOLO、CAN、网卡） |
| `config/follow.conf` | 跟随控制器 PID 与限幅 |
| `config/car_motor_can.conf` | 电机 CAN 协议映射（未配置时无输出） |
| `config/cpu_freq.conf` | CPU 调频策略 |
| `/etc/proxy-client.conf` | 整机代理，是 `proxy-client.service` 的 EnvironmentFile；开关即启停 |

## 模型

`config/yolo26.rknn`（RK3576 平台），`scripts/model_sync.sh` 保证仓库与部署目录一致；
网页可上传替换 `.rknn` + `.names`。

## Codex CLI 集成

`scripts/codex_bridge.py`：HTTP 层与 Codex CLI 之间的短生命周期桥。

- 登录 `codex login --device-auth`（PTY 抓设备码/链接，复制验证码走 `execCommand` 回退，因板子是 http 非安全上下文）
- 对话 `codex exec --json`，prompt 走 stdin，输出落 `recordings/codex/`
- 额度/模型：`codex app-server`（JSON-RPC `account/rateLimits/read`）与 `codex debug models`
- 升级：`systemd-run` 起独立单元在宿主命名空间安装（服务沙箱写不了 `~/.codex`），
  后台采样下载字节写入 `recordings/codex/codex_update_progress.txt`，前端每 2.5 秒轮询

交互记录为人类可读纯文本（`chat_YYYYMMDD.txt`）：

```
23:18:38 你：你好
23:19:04 Codex：……
23:19:20 错误：……
```

旧版 JSON-Lines 记录在首次运行时就地迁移成该格式。

## 前端约定

- 设计令牌集中在 `css/common.css`（颜色/间距/字阶/控件高度），新增样式只用变量
- 胶囊状的度量文字统一 `11.5px`、`--muted`、`tabular-nums`，超宽用省略号
- 主操作实心（保存），次操作描边（测试/上传/检查更新），危险操作红色（重启/关机）
- 改 CSS 时**必须**保留原文件尾部（历史上出现过截断 12 880 → 3 565 字节导致整页失样式）
- 部署后先自检：`curl` 静态资源应为 200 且字节数与磁盘一致，再交付

## 许可证

GPL-3.0-or-later，见 `LICENSE`。
