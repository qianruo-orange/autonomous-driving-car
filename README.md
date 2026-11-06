# 自动驾驶小车 · Autonomous Driving Car

> Radxa ROCK 4D（RK3576）视觉循迹小车：摄像头采集 → NPU 推理 → 目标跟踪 → CAN 电机输出，
> 内置 Web 控制台。A vision-guided car on Radxa ROCK 4D (RK3576): camera → NPU inference →
> tracking → CAN motor, with a built-in web console.
>
> 中文完整手册在前，English manual follows.

---

# 中文手册

## 1. 这是什么

单进程 C 服务把「采集 / 推理 / 控制 / 推流 / Web」串成一条流水线，实车运行在 Armbian（Debian 13）。
网页端可完成全部日常操作：看画面、调参数、换模型、开关代理、跟 Codex 对话。

**数据流**

```
UVC 摄像头 ──V4L2──> 采集线程 ──RGA 预处理──> NPU 推理池 ──> 检测结果(带 seq)
                          │                                   │
                          ├──MPP 编码──> MJPEG 推流 ──> 浏览器画面
                          └──> 跟踪器 ──> PID ──> CAN FD ──> 电机
```

时间轴以采集序号 `seq` 对齐：画面与检测框严格同帧，端到端延迟可在页面底部看到。

## 2. 硬件与系统

| 项目 | 规格 |
|---|---|
| 主板 | Radxa ROCK 4D（RK3576，8 核 4×A72@2.2G + 4×A53@2.0G） |
| 内存 / 存储 | 6 GB LPDDR5 / 476 GB NVMe（`/dev/nvme0n1p1`） |
| 系统 | Armbian 26.8 / Debian 13 trixie，aarch64 |
| 内核 | `7.4.0-rc1-npu-rt-full`（PREEMPT_RT + RKNPU，自编译） |
| 摄像头 | UVC，默认 `/dev/video2`，1280×720@30 |
| 执行器 | CAN FD 电机（未配置时不输出任何报文） |
| 加速器 | NPU `renderD128`、GPU `renderD129`、RGA `/dev/rga` |

## 3. 仓库结构

```
source/core        配置读写、日志
source/http        HTTP 服务与全部 API
source/video       V4L2 采集、MPP 编码、MJPEG 推流
source/yolo        RKNN 推理池（多线程 + RGA 预处理）
source/control     跟随控制器（PID、死区、限幅）
source/can         CAN FD 收发与电机协议映射
source/telemetry   匿名助手（ANO）链路
include/           头文件
html/              前端静态页（无构建步骤）
scripts/           build.sh / deploy.sh / model_sync.sh / proxy_config.py / codex_bridge.py / git_sync.sh
config/            运行期配置（部署时同步，可被网页修改）
docs/              设计、性能与测试文档（见第 12 节）
```

服务单元 `/etc/systemd/system/autonomous-driving-car.service`，运行目录 `/opt/autonomous-driving-car`，
沙箱：`ProtectSystem=strict`、`ProtectHome=read-only`、`PrivateTmp=yes`，可写路径仅
`config/`、`recordings/`、`.codex/`、`/etc/proxy-client.conf`。

> 这条约束很重要：服务内**写不了** `~/.codex`，所以 Codex 升级必须借 `systemd-run` 逃到宿主命名空间执行。

## 4. 首次上手（从零到能跑）

### 4.1 依赖

```bash
sudo apt install -y build-essential cmake git python3 curl
# RKNN Runtime / MPP / librga 由板厂镜像或 kbuild 提供，可用以下命令确认
ls /usr/local/lib/librga.so /dev/rga /dev/dri/renderD128
```

### 4.2 编译

```bash
cd ~/autonomous-driving-car
./scripts/build.sh -R          # 预期末行： [BUILD] done -> .../bin/autonomous-driving-car
```

### 4.3 部署

```bash
sudo ./scripts/deploy.sh -i    # 预期末行： === deploy complete ===
systemctl status autonomous-driving-car --no-pager
```

### 4.4 验证

```bash
curl -s -u qianruo:qianruo http://127.0.0.1/api/system | head -c 200
curl -s -u qianruo:qianruo http://127.0.0.1/api/video/devices
```

浏览器打开 `http://192.168.1.7/`，应看到实时画面与右上角 FPS 徽标。

### 4.5 提交同步

```bash
./scripts/git_sync.sh "一句话说明"
```

## 5. 网页控制台

### 5.1 主界面 `/`

1. 打开页面 → 自动连接 MJPEG 流（右上角显示 FPS、底部显示采集/处理/端到端延迟）
2. 画面里出现识别框 → **点击框**开始追踪；再点一次取消
3. 右侧「Codex 对话」可直接提问（需先在 `/config` 登录）

### 5.2 系统状态 `/status`

四张卡：CPU（逐核 + 温度）、NPU/RGA、内存/磁盘、网络速率。数值每 1 秒刷新。

### 5.3 系统配置 `/config`

| 卡片 | 操作 |
|---|---|
| YOLO 检测 | 置信度 / NMS；上传 `.rknn` 与 `.names` → 点「上传」→「保存」 |
| CPU 频率 | 选策略组与调度器，设最低/最高频率 →「保存」 |
| 网络代理 | 开关即时启停整机代理；改主机/端口/协议 →「保存」；「测试」「测速」结果显示在标题栏 |
| Web 与匿名助手 | 网卡、IP 模式、ANO 端口与发送频率 →「保存」 |
| 视频采集 | 选设备与分辨率 →「保存」 |
| 智能跟随 | 控制状态（停止/启用）、目标、速度策略、PID 分组保存 |
| Codex CLI | 登录/退出、模型与推理速率、可用额度、检查更新/安装更新 |

> 首次进入若弹出「需要管理员权限」，输入 `root` 或 `sudo` 组账号（密码同板子登录密码）；
> 该凭据存于当前标签页 `sessionStorage`，只用于写接口。

### 5.4 文件下载 `/logs`

日志、录像、检测结果、Codex 交互记录分类列出，可预览、打包下载、清空。

### 5.5 DBC 解析 `/dbc`

上传 `.dbc` 后实时解析 CAN 报文，可查看信号值变化。

## 6. CAN 与电机

1. `/config` → 顶部「启用 CAN」→ 选择接口与波特率
2. 电机协议需自行配置：把 `config/car_motor_can.example.conf` 复制为 `config/car_motor_can.conf`
3. 按电机手册填写 `cmd_id / offset / width / scale / stop_template` 等字段
4. `enabled 1` → 重启服务 → 在「智能跟随」里把控制状态设为「启用跟随」

**安全提示**：`enabled 0` 时控制器不发送任何报文；先确认电机驱动的独立看门狗再上车测试。

## 7. 模型替换

- 网页上传：`/config` → YOLO → 选择 `.rknn` 与 `.names` →「上传」
- 命令行：替换 `config/yolo26.rknn` 后执行 `sudo ./scripts/deploy.sh -i`（`model_sync.sh` 会校验一致性）
- 量化流程见 [docs/YOLO26N_QUANTIZATION.md](docs/YOLO26N_QUANTIZATION.md)、[docs/YOLO26n量化报告.pdf](docs/pdf/YOLO26n量化报告.pdf)

## 8. Codex 集成

| 动作 | 路径 | 说明 |
|---|---|---|
| 登录 | `/config` → Codex →「登录」 | 设备码流程，15 分钟内有效；复制按钮已兼容 http 非安全上下文 |
| 对话 | 主界面右侧 | `codex exec --json`，prompt 走 stdin，后台执行不阻塞网页 |
| 额度 | Codex 卡片 | `app-server` 的 `account/rateLimits/read`，缓存 60 秒 |
| 模型 | Codex 卡片 | `codex debug models`，只列可见且可用的模型，缓存 1 小时 |
| 升级 | 「安装更新」 | `systemd-run` 后台单元 + 进度轮询（下载字节/阶段） |

交互记录是纯文本 `recordings/codex/chat_YYYYMMDD.txt`：

```
23:18:38 你：你好
23:19:04 Codex：……
23:19:20 错误：……
```

## 9. 静态资源与缓存策略

- 页面引用带版本号：`/css/config.css?v=2026110701`
- 服务端 `http_serve_file()` 入口剥掉 `?query` 再找文件（`source/http/http.c`）
- 响应头 `Cache-Control: no-cache`；**禁止**改成 `no-store`（会让每次切页全量重下，明显变卡）
- 部署后要立即生效：修改 `finish_two.sh` 里的 `VER` 重新生成版本号

## 10. 运行与排障

```bash
systemctl status autonomous-driving-car      # 服务状态
journalctl -u autonomous-driving-car -f      # 实时日志
curl -s -u qianruo:qianruo http://127.0.0.1/api/video/stats
```

| 现象 | 排查 |
|---|---|
| 画面黑屏 | `/api/video/devices` 是否有设备；`fuser /dev/video2` 是否被占用 |
| 检测框不出现 | `/api/detections/live` 是否有结果；模型文件与 `.names` 是否匹配 |
| 页面样式全丢 | `curl /css/config.css` 应为 200 且字节数与磁盘一致；历史上出现过 CSS 被截断 |
| 保存报 401 | 管理员凭据失效，重新弹框输入 `root`/sudo 账号 |
| 跟随不动 | `config/car_motor_can.conf` 未配置或 `enabled 0`；CAN 是否已启用 |
| 更新卡住 | `releases.openai.com` 在此链路不通，脚本会回退 GitHub；187 MB 下载很慢 |

## 11. 前端规范

- 设计令牌集中在 `css/common.css`（间距/圆角/控件高度/字阶/语义色），新样式只用变量
- 度量文字统一 11.5px、`--muted`、`tabular-nums`，超宽省略
- 主操作实心、次操作描边、危险操作红色
- 改 CSS **必须**保留文件尾部（曾把 12 880 字节写成 3 565 字节，导致整页失样式）
- 部署后先 `curl` 自检静态资源字节数，再交付

## 12. 设计文档

| 文档 | 内容 |
|---|---|
| [docs/CODE_REVIEW.md](docs/CODE_REVIEW.md) | 代码评审记录与已知问题 |
| [docs/COMMUNICATION_FLOW.md](docs/COMMUNICATION_FLOW.md) | 线程/模块间通信与队列设计 |
| [docs/FRAME_PIPELINE.md](docs/FRAME_PIPELINE.md) | 帧流水线：采集 → 推理 → 渲染的时序 |
| [docs/LATENCY_PROFILE.md](docs/LATENCY_PROFILE.md) | 端到端延迟剖析与优化 |
| [docs/REALTIME_SCHEDULING.md](docs/REALTIME_SCHEDULING.md) | 实时调度（PREEMPT_RT、FIFO、CPU 隔离） |
| [docs/FUNCTIONAL_TEST_REPORT.md](docs/FUNCTIONAL_TEST_REPORT.md) | 功能测试报告 |
| [docs/YOLO26N_QUANTIZATION.md](docs/YOLO26N_QUANTIZATION.md) | YOLO26n RKNN 量化全过程 |
| [docs/推理流水线延迟剖析.pdf](docs/pdf/推理流水线延迟剖析.pdf) | 延迟剖析（PDF） |
| [docs/YOLO26n量化报告.pdf](docs/pdf/YOLO26n量化报告.pdf) | 量化报告（PDF） |

---

# English Manual

## 1. Overview

A single C process runs the whole pipeline — capture, NPU inference, tracking, CAN output, MJPEG
streaming and the web console — on Armbian (Debian 13). Everything day-to-day is done in the browser.

**Data flow**

```
UVC camera ─V4L2→ capture thread ─RGA preprocess→ NPU pool → detections (tagged with seq)
                        │                                      │
                        ├─MPP encode→ MJPEG stream ─→ browser
                        └→ tracker → PID → CAN FD → motor
```

Frames and detections share the capture sequence `seq`, so overlays are strictly frame-aligned and the
end-to-end latency is shown at the bottom of the main page.

## 2. Hardware

| Item | Spec |
|---|---|
| Board | Radxa ROCK 4D (RK3576, 4×A72@2.2 GHz + 4×A53@2.0 GHz) |
| RAM / disk | 6 GB LPDDR5 / 476 GB NVMe |
| OS | Armbian 26.8 (Debian 13 trixie), aarch64 |
| Kernel | `7.4.0-rc1-npu-rt-full` (PREEMPT_RT + RKNPU, built in-house) |
| Camera | UVC, default `/dev/video2`, 1280×720@30 |
| Actuator | CAN FD motor driver (no output until configured) |
| Accelerators | NPU `renderD128`, GPU `renderD129`, RGA `/dev/rga` |

## 3. Getting started

```bash
sudo apt install -y build-essential cmake git python3 curl
./scripts/build.sh -R                 # → bin/autonomous-driving-car
sudo ./scripts/deploy.sh -i           # → /opt/autonomous-driving-car, service restarted
curl -s -u qianruo:qianruo http://127.0.0.1/api/system | head -c 200
./scripts/git_sync.sh "your message"  # commit + push
```

Open `http://192.168.1.7/` — the live MJPEG stream and an FPS badge should appear.

## 4. Web console

| Page | Purpose |
|---|---|
| `/` | Live stream, click a box to start tracking, Codex chat |
| `/status` | CPU per-core + temperature, NPU/RGA, memory/disk, network throughput |
| `/config` | YOLO, CPU frequency, network proxy, Web/ANO, camera, follow controller, Codex CLI |
| `/logs` | Logs, recordings, detections, Codex transcripts — preview, pack, clear |
| `/dbc` | DBC parsing of live CAN frames |

Write endpoints are protected by Basic Auth; the first write from a fresh tab opens an in-page
admin dialog (`root` or any `sudo` group account). The credential lives in `sessionStorage`.

## 5. CAN and motor

1. `/config` → **Enable CAN** → pick interface and bitrate.
2. Copy `config/car_motor_can.example.conf` to `config/car_motor_can.conf`.
3. Fill `cmd_id / offset / width / scale / stop_template` from the motor manual.
4. Set `enabled 1`, restart the service, then set the follow controller to **Enable follow**.

With `enabled 0` the controller transmits nothing. Configure the drive's independent watchdog before
any on-vehicle test.

## 6. Models

Upload via `/config` → YOLO, or replace `config/yolo26.rknn` and re-run `deploy.sh -i`
(`model_sync.sh` reconciles repo and deployment). Quantisation details:
[docs/YOLO26N_QUANTIZATION.md](docs/YOLO26N_QUANTIZATION.md).

## 7. Codex integration

- **Sign in**: device-code flow, valid for 15 minutes (clipboard copy falls back to `execCommand`
  because the board is served over plain HTTP).
- **Chat**: `codex exec --json`, prompt on stdin, detached so the web worker never blocks.
- **Quota**: `codex app-server` → `account/rateLimits/read`, cached for 60 s.
- **Models**: `codex debug models`, visible + API-capable entries only, cached for an hour.
- **Upgrade**: a detached `systemd-run` unit (the service sandbox cannot write `~/.codex`), progress
  polled from the download byte count.

Transcripts are plain text under `recordings/codex/`:

```
23:18:38 你：你好
23:19:04 Codex：……
23:19:20 错误：……
```

## 8. Static assets and caching

Versioned URLs (`?v=2026110701`); the server strips the query string in `http_serve_file()`.
`Cache-Control: no-cache` — never `no-store`, which forces a full re-download on every navigation.
Bump `VER` in `finish_two.sh` after a deploy.

## 9. Troubleshooting

| Symptom | Check |
|---|---|
| Black preview | `/api/video/devices`; `fuser /dev/video2` |
| No detection boxes | `/api/detections/live`; model/names mismatch |
| Styles gone | `curl /css/config.css` must be 200 with matching byte size |
| 401 on save | Re-enter admin credentials in the dialog |
| Car does not move | `car_motor_can.conf` missing or `enabled 0`; CAN disabled |
| Update stalls | `releases.openai.com` is unreachable here; the script falls back to GitHub, 187 MB is slow |

## 10. Frontend conventions

Design tokens live in `css/common.css` (spacing, radius, control height, type scale, semantic colours).
Numeric readouts use `tabular-nums`. Primary actions are filled, secondary outlined, destructive red.
When editing CSS, always keep the file tail — a truncated write once stripped 9 KB and broke the page.

## 11. Design documents

| Document | Contents |
|---|---|
| [docs/CODE_REVIEW.md](docs/CODE_REVIEW.md) | Code review notes and known issues |
| [docs/COMMUNICATION_FLOW.md](docs/COMMUNICATION_FLOW.md) | Inter-thread/module communication and queues |
| [docs/FRAME_PIPELINE.md](docs/FRAME_PIPELINE.md) | Frame pipeline timing |
| [docs/LATENCY_PROFILE.md](docs/LATENCY_PROFILE.md) | End-to-end latency analysis |
| [docs/REALTIME_SCHEDULING.md](docs/REALTIME_SCHEDULING.md) | PREEMPT_RT, FIFO scheduling, CPU isolation |
| [docs/FUNCTIONAL_TEST_REPORT.md](docs/FUNCTIONAL_TEST_REPORT.md) | Functional test report |
| [docs/YOLO26N_QUANTIZATION.md](docs/YOLO26N_QUANTIZATION.md) | YOLO26n RKNN quantisation walkthrough |
| [docs/推理流水线延迟剖析.pdf](docs/pdf/推理流水线延迟剖析.pdf) | Latency profile (PDF) |
| [docs/YOLO26n量化报告.pdf](docs/pdf/YOLO26n量化报告.pdf) | Quantisation report (PDF) |

---

## License / 许可

GPL-3.0-or-later — see [LICENSE](LICENSE).
