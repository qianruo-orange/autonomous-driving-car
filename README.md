# rockchip-smart-gateway

基于 epoll 的 CAN / CAN FD 数据采集、DBC 信号解析与 TCP/IP 通信解决方案，内置 Web 管理界面、视频流、AI 目标检测（RKNN + YOLO26）、网络录像与 systemd 看门狗。**全面适配 Rockchip 全系 RKNPU 平台**（RK3588 / RK3576 / RK356x / RV1126 / RV110x / RK3399PRO / RK1808 等），NPU / CPU / GPU 等硬件资源的核数与负载均按平台真实情况自动识别与展示。适用于 Rockchip（如 Radxa Rock 4D、Orange Pi 5 系列）等运行 Linux 的边缘设备，使用 C11 实现。

## 功能特性

- CAN 数据采集：支持经典 CAN 与 CAN FD，多接口管理，自动识别系统 CAN 接口
- DBC 信号解析：解析 DBC 数据库，将 CAN 帧解码为物理量信号（接收 / 发送双方向）
- TCP/IP 通信：TCP 服务监听，端口与绑定网卡均可配置
- Web 管理界面：数据监控、配置修改、日志查看与重启 / 关机控制
- 视频流服务：通过 V4L2 接入摄像头并输出 MJPEG 视频流
- 视频帧环形队列：采集 / 推理 / 显示 / 编码四游标 + 分阶段缓冲池复用，V4L2 mmap 指针零拷贝入环（整帧只移动指针），内置分阶段丢帧计数器，仅凭计数器即可定位丢帧环节
- AI 目标检测：RKNN + YOLO26（官方 ultralytics rknn 单输出格式），多线程推理池并行加速，检测结果实时画框推流
- **平台自适应 NPU 调度（Rockchip 全系）**：按 SoC 实际 NPU 核心数自动选择合法核掩码，RKNN `rknn_set_core_mask` 不再报 `unavailable core mask`，多核自动分摊推理负载（RK3588 三核 / RK3576 双核 / RK356x 等单核平台均兼容）
- **前端 NPU / CPU 核数自适应**：仪表盘按后端上报的真实核数动态渲染核格 —— 数量随平台变化、尺寸随数量自动缩放（核少大格、核多自动换行）
- **GPU 监控自适应**：优先读取 GPU 负载（RK3588 等暴露 `load` 的驱动）；RK3576 / RK356x 等驱动未暴露 `load` 时，自动回退显示 GPU 当前运行频率，保证 GPU 卡片在所有平台都有真实信息
- 网络录像：默认自动开启录屏（按天分目录 `recordings/YYYYMMDD/`），Rockchip 硬件编码为 H.264 并封装 MP4(avc1) 文件，分辨率取自摄像头配置
- systemd 看门狗：监控关键线程心跳，防止异常卡死
- 热更新配置：Web 页面修改后立即生效
- 运行维护：日志按日期与大小自动轮转（按天分目录）、清理脚本、部署脚本
- 数据注入 / 提取：REST API 提供 CAN 报文注入（`/api/can/send`）、DBC 上传热加载、配置写入，以及视频流 / CAN 原始帧与 DBC 解码信号 / 录像 / 日志等数据提取

## 系统架构

单进程多线程架构，所有线程由 `source/core/main.c` 的模块总表 `g_modules[]` 统一注册并拉起：

```text
        ┌─► can_recv_task ──► SocketCAN 收帧 ──► 入 rxq（原始帧）+ 接收方向 DBC 解码
 main ──┼─► can_send_task ──► 排空 txq（原始帧）写 socket + 发送方向 DBC 解码
        ├─► tcp_task      ──► TCP 监听 / 收发
        ├─► http_task     ──► Web 管理 + REST API + MJPEG 推流
        ├─► video_task    ──► V4L2 采集（mmap 零拷贝入环形队列）
        ├─► ai_task       ──► 事件驱动消费环槽 → 推理队列（多线程池并行推理）→ rgb 写槽 + NMS 结果入结果队列
        ├─► rec_task      ──► 网络录像（零拷贝取显示帧 NV12，AI 停摆直接宕机）→ H.264 硬件编码 → MP4(avc1)
        └─► watchdog_task ──► 心跳监控 + sd_notify
```

| 线程 | 职责 |
| --- | --- |
| `main` | 初始化、配置加载、DBC 加载、退出编排、喂狗 |
| `can_recv` | CAN 帧接收、入 rxq（原始帧）、接收方向 DBC 解码 |
| `can_send` | 排空 txq（原始帧）写 socket、发送方向 DBC 解码 |
| `tcp` | TCP 监听、连接管理与数据收发 |
| `http` | HTTP 管理服务主循环、REST API、MJPEG 推流 |
| `video` | V4L2 采集：mmap 指针零拷贝入环形队列，推进 produce 游标 |
| `ai` | 事件驱动消费环槽（infer 游标）、推理任务投递；模型加载与多线程推理池（每线程独立 rknn context，按 `i % 实际NPU核数` 绑定核）；推理后分两路：rgb 写槽走显示链路，NMS 结果（名称+置信度+坐标）入结果队列供 composer 消费 |
| `rec` | 网络录像：从环取显示帧 NV12 直接编码（encode 游标；AI 停摆 >1s 置 fatal 整体宕机）→ H.264 硬件编码 → MP4(avc1) 封装落盘 |
| `watchdog` | 线程心跳监控与 `sd_notify` |

> 注：`signal` 模块仅注册信号处理（`init`），`task` 为空，不创建线程。

## 数据流向与接口

### 视频流向

```text
V4L2 摄像头（MJPEG / YUYV）
  └► video_task 采集：mmap 指针零拷贝入环 ──► produce 游标推进
        └► ai_task 事件驱动取槽（infer 游标）─► worker×N 并行推理（RKNN）─► rgb 写槽
                    ├► NMS 结果（名称+置信度+坐标）入结果队列 ─► composer 消费最新结果画框（display 游标）
                    ├► http 逐连接推流 ─► /video/mjpeg_ai（AI 必要流程，不可用返回 503）
                    └► rec_task 取显示帧 NV12（encode 游标）─► H.264 硬件编码 ─► MP4(avc1) 落盘
原始流 /video/mjpeg 由 http 直接从最新采集槽拷贝推流（与推理链路并行）
```

- 帧环形队列：32 槽 × 四阶段缓冲（raw / rgb / nv12 / jpeg），四游标（produce / infer / display / encode）单调推进；缓冲按需分配、游标越过后回池复用——整帧只移动指针，无逐帧 malloc / memcpy
- 采集：`video_task` V4L2 mmap 缓冲 DQBUF 后指针直接进槽（延迟 QBUF 至消费完成，真正零拷贝），环满才丢弃（capture_dropped）；优先 MJPEG，失败回退 YUYV；分辨率取 `video_width` / `video_height`
- 推理：`ai_task` 阻塞等待 produce 游标（事件驱动），取槽随任务投递 worker（任务存续期间钉住 raw 槽，采集不回收），解码 RGB 写入槽；推理后数据流分两路——rgb 写槽走显示链路，NMS 后处理结果（目标名称 + 置信度 + 标注框四点坐标）入结果队列（纯元数据，不携带缓冲）
- 显示：composer 事件驱动消费结果队列（排空只留最新，旧结果丢弃计 render_skipped），按 seq 定位 rgb_done 槽画框写 nv12 / jpeg 进槽；每个 `/video/mjpeg*` 连接一个独立线程各自拷贝最新 JPEG 推流（multipart/x-mixed-replace），慢客户端不钉槽、不阻塞其他连接
- 编码：`rec_task` 从 encode 游标零拷贝窃取显示帧 NV12（用毕归还缓冲池）；与编码器分辨率不符时跳过（encode_skipped）
- AI 必要流程：模型缺失 / NPU 不可用时启动直接失败退出（systemd failed），`/video/mjpeg_ai` 返回 503，不回退原始帧；运行期 AI 停摆 >1s（无新标注帧）不回退原始帧，直接整体宕机（退出码 1 → systemd Restart=on-failure 拉起）

视频数据提取接口：

| 接口 | 说明 |
| --- | --- |
| `GET /video/mjpeg` | 原始 MJPEG 视频流（浏览器 `<img src>` 或 ffplay 播放） |
| `GET /video/mjpeg_ai` | AI 画框视频流（检测结果实时叠加） |
| `GET /api/video/caps` | 摄像头能力（分辨率 / 格式） |
| `GET /api/video/stats` | 环形队列分阶段计数器（produce_seq / 各阶段丢帧 / 缓冲池占用），回答"摄像头 vs 推理"丢帧问题 |
| `GET /api/video/devices` | 可用视频设备列表 |

### CAN 数据流向

```text
接收：SocketCAN 帧 ─► can_recv_task（epoll 非阻塞读）
        ├► 原始帧入 rxq（供业务 pop 消费）
        ├► http_can_record_rx ─► /api/can/frames（最近 32 帧）
        └► 接收方向 DBC 解码 ─► /api/can/decoded（最近信号值）

发送：can_tx_frame() 压入 txq ─► eventfd 唤醒 can_send_task ─► 排空写 socket
        ├► 发送方向 DBC 解码 ─► /api/can/decoded/tx
        └► 写失败（EAGAIN）回队尾，队列满记录丢帧
```

- 队列：每接口独立 rxq / txq（原始 `canfd_frame`），DBC 解码与原始帧缓存相互独立
- 故障恢复：读错误自动重建 socket 并重新绑定过滤器；接口断开不阻塞整体启动

CAN 数据注入接口（写数据到总线）：

| 接口 | 说明 |
| --- | --- |
| `POST /api/can/send` | 注入一帧 CAN/CAN FD 报文（body：`ifname=can0&id=0x123&data=01 02 03`，支持 0x 前缀、空格/逗号/连字符分隔） |
| `POST /api/can/toggle` | 控制接口 up / down（body：`ifname=can0&action=up`） |
| `POST /api/can/dbc` | 上传 DBC 数据库（`?ifname=can0`，body 为 DBC 文本，校验通过后热加载并落盘） |
| `POST /api/config` | 写入配置（body 带 `target` 指定模块：`can` / `net` / `video` / `ai`，只应用该模块参数并重启对应模块；缺省为全量） |

CAN 数据提取接口（从总线读数据）：

| 接口 | 说明 |
| --- | --- |
| `GET /api/can` | CAN 接口状态（up / 波特率 / FD） |
| `GET /api/can/ifaces` | 系统 CAN 接口枚举（含 FD 能力） |
| `GET /api/can/frames` | 最近原始接收帧（id / len / data / 标志） |
| `GET /api/can/decoded` | 接收方向 DBC 解码信号记录 |
| `GET /api/can/decoded/tx` | 发送方向 DBC 解码信号记录 |

### TCP 数据流向

```text
监听：tcp_task 监听配置端口（tcp_port，可选绑定网卡 tcp_bind）
        ├► accept 新连接 ─► 注册 epoll（每客户端独立读写状态）
接收：客户端数据 recv 读取（当前版本读取后丢弃，不注入 CAN 总线）
发送：业务线程压入 txq（tcp_tx_packet） ─► eventfd 唤醒 tcp_task ─► 写客户端 socket
        └► 支持定向发送（client_idx）与广播（client_idx < 0）；慢客户端由每客户端 wbuf 暂存
```

- TCP 服务面向外部客户端：连接数上限 `max_clients`（默认 16），非阻塞 + epoll 管理，部分写由 wbuf 续发
- 发送链路（TX 队列 + eventfd + 广播/定向分发）已就绪，供后续业务注入（如 CAN 帧转发到 TCP 客户端）；当前版本 TCP 接收方向不转发数据

### 系统监控接口

`GET /api/system` 返回 CPU / 内存 / 磁盘 / 温度 / 网络速度 / NPU / GPU 等实时数据：

| 字段 | 说明 |
| --- | --- |
| `cpu_cores` | 每个 CPU 核心的累计使用率（数组，长度 = 真实核心数） |
| `npu_count` / `npu_cores` | NPU 核心数及各核负载（%，-1 表示缺失）；核数由内核 `debugfs` 实际解析，自动匹配 RK3588 三核 / RK3576 双核 / 单核 |
| `gpu` | GPU 负载百分比（%）。**仅 RK3588 等暴露 `load` 的驱动可用**，其余平台为 -1 |
| `gpu_freq` | GPU 当前运行频率（MHz）。当驱动未暴露 `load` 时（如 RK3576 / RK356x），以此作为 GPU 卡片的真实指标回退显示 |

## 平台适配（Rockchip 全系 RKNPU）

本项目的 NPU 调度与硬件展示均**按平台实际情况自适应**，无需针对具体 SoC 改代码：

- **NPU 核掩码自动适配**：`source/ai/rknn_yolo.c` 启动时读 `/proc/device-tree/compatible` 推断 NPU 核心数（RK3588/3588S=3、RK3576/3576M=2、RK356x / RV1126 / RV110x / RK3399PRO / RK1808=1，未知安全回退单核），仅使用 ≤ 合法上限的单核掩码（`1u << (idx % cores)`），根除 `rknn_set_core_mask: unavailable core mask` 运行时报错，并保留多核分摊。
- **AI 推理线程数按 NPU 核数自适应**：`ai_threads` 配置项会被取整为「NPU 核数的整数倍」（范围 `cores~15`，默认每核 1 个），各核等量分配 worker；不再写死为 3 的倍数（旧版仅适配 RK3588 三核）。
- **前端 NPU / CPU 核格自适应**：仪表盘按 `npu_count` / `cpu_cores.length` 动态生成核格，数量随平台变化、尺寸随数量缩放（核少单排大格、核多自动换行）。
- **GPU 监控自适应**：见上 `gpu` / `gpu_freq` 说明。
- **模型按平台同步**：`build.sh` 在编译前自动按平台（`/proc/device-tree/compatible` 或 `-P` 指定）选取/转换对应 RKNN 模型（如 `config/models/rk3576/yolo26.rknn`），详见下文「构建」。

## 目录结构

```text
rockchip-smart-gateway/
├── CMakeLists.txt
├── README.md
├── rockchip-smart-gateway.service  # systemd 服务单元
├── include/                          # 头文件
│   ├── core/
│   │   ├── common.h                  # 应用上下文与公共工具
│   │   ├── config.h                  # 配置结构
│   │   ├── log.h                     # 日志接口
│   │   ├── ring.h                    # 无锁环形队列
│   │   ├── epoll_util.h              # epoll 辅助工具
│   │   └── version.h                 # 版本宏（CMake 生成，gitignore）
│   ├── can/
│   │   ├── can_queue.h               # CAN 收发队列
│   │   ├── can_socket.h              # SocketCAN 接口
│   │   └── dbc_parser.h              # DBC 解析接口
│   ├── tcp/
│   │   ├── tcp_server.h              # TCP 服务接口
│   │   └── tcp_queue.h               # TCP 发送队列
│   ├── http/
│   │   ├── http.h                    # HTTP 服务接口
│   │   ├── http_internal.h           # HTTP 内部定义
│   │   ├── http_util.h               # HTTP 表单/URL 解析工具
│   │   ├── http_api_can.h            # CAN API 接口声明
│   │   └── http_api_dbc.h            # DBC API 接口声明
│   ├── video/
│   │   ├── video_stream.h            # V4L2 视频流接口
│   │   ├── frame_ring.h              # 视频帧环形队列（四游标零拷贝 + 分阶段丢帧计数）
│   │   ├── video_rec.h               # 网络录像模块接口
│   │   ├── h264_encoder.h            # Rockchip 硬件 H.264 编码器封装接口（V4L2 M2M rkvenc）
│   │   └── rec_mp4.h                 # MP4(avc1) 封装器接口
│   ├── ai/
│   │   ├── rknn_yolo.h               # RKNN YOLO 框架入口（含 npu_core_count 声明）
│   │   ├── yolo_types.h              # 检测结果结构定义
│   │   ├── yolo_image.h              # 图像处理接口（JPEG / YUYV / letterbox）
│   │   ├── yolo_postprocess.h        # 单输出后处理接口
│   │   └── yolo_draw.h               # 画框接口
│   └── watchdog/
│       └── watchdog.h                # 看门狗接口
├── source/                           # 源代码
│   ├── core/      main.c / config.c / log.c
│   ├── can/       can_queue.c / can_socket.c / dbc_parser.c
│   ├── tcp/       tcp_server.c / tcp_queue.c
│   ├── http/      http.c / http_util.c / http_api_*.c（can/dbc/config/network/system/video/rec/ai/logs/reboot）
│   ├── video/     video_stream.c / frame_ring.c / video_rec.c / h264_encoder.c / rec_mp4.c
│   ├── ai/        rknn_yolo.c / yolo_image.c / yolo_postprocess.c / yolo_draw.cpp
│   └── watchdog/  watchdog.c
├── tests/        frame_ring_test.c
├── html/         index.html / config.html / dbc.html / logs.html + css/ + js/
├── config/       运行时配置（config.txt 为设备专属，已 gitignore；另含 can0.dbc / can1.dbc 模板）
├── docs/         文档与截图
├── scripts/     build.sh / deploy.sh
├── bin/         可执行输出（构建生成，gitignore）
├── build/       CMake 构建目录（构建生成，gitignore）
├── logs/        运行日志（运行生成，gitignore）
├── recordings/  网络录像输出（运行生成，gitignore）
└── .gitignore
```

## 依赖要求

- gcc / g++（C11 / C++11，检测框渲染模块为 C++ 并直接调用 OpenCV）
- cmake ≥ 3.10
- libsystemd
- libnl-3 / libnl-route-3
- librknnrt（板载 RKNN 运行时，模型由 rknn-toolkit2 转换，支持板端或 PC）
- libjpeg（JPEG 解码与 AI 画框用）
- OpenCV 4（core + imgproc，检测框画框与标签文字渲染）
- Rockchip 硬件编码器 `/dev/video-enc0`（rkvenc，Linux V4L2 M2M 驱动，录像 H.264 编码用）
- Linux SocketCAN 支持

安装示例：

```bash
apt install build-essential cmake libsystemd-dev libnl-3-dev libnl-route-3-dev libjpeg62-turbo-dev libopencv-dev
```

## 构建

```bash
cd /home/qianruo/rockchip-smart-gateway
./scripts/build.sh            # Release（默认），自动辨识当前板卡 RKNPU 平台
./scripts/build.sh -D         # Debug
./scripts/build.sh -P rk3576  # 显式指定目标平台（覆盖自动辨识，如交叉部署场景）
./scripts/build.sh -C         # 清理构建产物与 logs
```

`build.sh` 构建前会自动检查依赖（工具链 + 开发库 + librknnrt），缺失时打印具体缺项与安装命令并中止。

**平台辨识与模型同步**（RKNPU 全系兼容）：
- 平台默认从 `/proc/device-tree/compatible` 自动辨识（如 `rockchip,rk3576` → `rk3576`），`-P` 可显式覆盖
- 编译前自动同步 `config/yolo26.rknn`：优先部署 `config/models/<平台>/yolo26.rknn`；运行模型平台不匹配或缺失时调用 ultralytics 官方 CLI（`yolo export model=<权重> format=rknn name=<平台>`）在板端自动转换，转换产物按平台存入 `config/models/` 复用

构建产物：`./bin/rockchip-smart-gateway`

版本信息由 CMake 在配置阶段生成到 `include/core/version.h`（含 `APP_NAME` / `APP_VERSION` / `APP_GIT_COMMIT` / `APP_GIT_BRANCH` / `APP_GIT_DIRTY` / `APP_BUILD_TYPE` / `APP_BUILD_DATE`），该文件为生成物，已加入 `.gitignore`。

## 运行方式

```bash
cd /home/qianruo/rockchip-smart-gateway
./bin/rockchip-smart-gateway
```

> 请在项目根目录运行，确保 `config/`、`html/` 和 `logs/` 目录可用。

## systemd 部署

前置条件：已通过 `./scripts/build.sh` 生成可执行文件。

```bash
sudo ./scripts/deploy.sh        # 安装并启动（默认动作，等同 -i）
sudo ./scripts/deploy.sh -i     # 安装并启动
sudo ./scripts/deploy.sh -u     # 卸载（停止服务、删除 systemd 文件与安装目录）
sudo ./scripts/deploy.sh -r     # 重装（卸载后重新安装）
sudo ./scripts/deploy.sh -h     # 查看帮助
```

`deploy.sh` 需 root 权限，安装目录为 `/opt/rockchip-smart-gateway/`，部署内容如下：

| 源 | 目标 |
| --- | --- |
| `bin/<可执行文件>` | `/opt/.../bin/` |
| `html/` | `/opt/.../html/` |
| `config/`（含 config.txt 与 DBC 模板） | `/opt/.../config/`（部署前先回拉设备端 config.txt，避免网页修改丢失） |
| systemd 服务单元 | `/etc/systemd/system/` |

> 注：`config/config.txt` 为设备专属运行配置（如 `video_device` 因板卡而异），已加入 `.gitignore`；`include/core/version.h` 为编译生成物，同样 gitignore。二者均不纳入版本库，避免每次构建/部署产生改动噪音。

服务名称：`rockchip-smart-gateway`

查看服务状态：

```bash
systemctl status rockchip-smart-gateway
journalctl -u rockchip-smart-gateway -f
```

部署要点：

- `Type=notify`
- `WatchdogSec=10`
- `Restart=on-failure`（失败自动重启，间隔 5s）
- `AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW`
- `ProtectSystem=strict`（文件系统只读，仅 `config/`、`logs/`、`recordings/` 可写，`html/` 只读）

## 配置说明

`config/config.txt` 由 Web 页面保存，并在运行时生效。

| 键 | 格式 | 说明 |
| --- | --- | --- |
| `can_ifname` | `<name>` | CAN 接口名称，可重复；未提供时自动从系统枚举实际存在的 CAN 接口 |
| `can_bitrate` | `<name> <bps>` | 比特率，默认 500000 |
| `can_dbitrate` | `<name> <bps>` | FD 数据比特率，默认 2000000 |
| `can_fd` | `<name> on\|off` | CAN FD 开关 |
| `can_up` | `<name> on\|off` | 启动时 bring up |
| `can_filter` | `<name> <id> <mask>` | 按 CAN 通道设置接收过滤器（16 进制 id / mask，可重复添加） |
| `tcp_port` | `<port>` | TCP 端口，默认 6666 |
| `max_clients` | `<n>` | 最大客户端数，默认 16 |
| `tcp_bind` | `<ifname>` | TCP 监听绑定网卡名，留空表示绑定所有网卡（`INADDR_ANY`） |
| `video_device` | `<path>` | 视频设备，默认 `/dev/video0` |
| `video_width` / `video_height` | `<n>` | 分辨率，默认 640×480 |
| `video_fps` | `<n>` | 期望帧率（1~120，V4L2 S_PARM 设置驱动帧间隔）；0 = 驱动默认 |
| `can_dbc` | `<name> <path>` | 按 CAN 通道配置 DBC 数据库文件路径，留空则不启用该通道信号解码 |
| `ai_model` | `<path>` | YOLO26 官方单输出 `.rknn` 模型路径，默认 `config/yolo26.rknn`（AI 推理为必要流程，始终启用；模型缺失/NPU 失败时启动直接失败退出） |
| `ai_names` | `<path>` | 类别标签文件路径（COCO 格式，每行一个类名），默认 `config/coco.names` |
| `ai_input_size` | `<n>` | 模型输入边长（32~2048），默认 640 |
| `ai_conf` | `<n>` | 置信度阈值百分比（1~100），默认 25（0.25） |
| `ai_nms` | `<n>` | NMS IoU 阈值百分比（1~100），默认 45（0.45） |
| `ai_interval_ms` | `<n>` | 推理节流间隔（10~5000ms），默认 10（每帧推理） |
| `ai_threads` | `<n>` | 推理工作线程数，**自动取整为 NPU 核数的整数倍（范围 `cores`~15，默认每核 1 个）**；每线程独立 rknn context，worker `i` 按 `i % 实际NPU核数` 绑定 NPU 核。配置页下拉框随平台 NPU 核数动态生成（如双核平台为 2/4/6/8/10/12/14，各标注「每核 N 个」） |

未提供配置文件时，将从系统枚举实际存在的 CAN 接口（netlink 路由，`kind=="can"`）并套用默认参数；若系统无法枚举到接口，则回退到 `can0` / `can1`。

### DBC 模板

`config/` 下提供两个 CAN 通道的 DBC 模板文件，可通过 `can_dbc <通道名> <路径>` 启用（或在 Web 配置页上传）：

- `config/can0.dbc`：动力 / 电池类（`EngineData`、`BatteryStatus`）
- `config/can1.dbc`：底盘 / 位置类（`Status` 含 Motorola 大端信号、`VehiclePos` 扩展帧）

> 解析器只识别 `BO_`（报文）与 `SG_`（信号）两类行，其余（含 `//` 注释）均忽略。ID 为十进制；扩展帧需在 29 位 ID 上置 bit31（如 `0x18FF50E5` → `2566869221`）。

## Web 管理界面

服务默认监听 HTTP 端口 80，提供以下入口：

- `/`：仪表盘（视频流下方实时显示客户端实测帧率徽标）
  - **CPU 卡片**：总体使用率 + 逐核核格（数量/尺寸随平台自适应）
  - **NPU 卡片**：逐核负载核格（`npu_count` 个，数量/尺寸随平台自适应）
  - **GPU 卡片**：优先显示负载百分比；驱动未暴露 `load` 时显示当前运行频率（MHz）并标注「负载不可用」
- `/dbc`：DBC 信号解析
- `/config`：运行配置
  - 视频卡：格式 / 分辨率 / 帧率合并为单一下拉框，按相机真实能力枚举（`/api/video/caps`），选择即保存并立即生效
  - AI 卡：推理参数 2×2 网格排列（无启用开关——AI 为必要流程），与模型文件上传左右分栏；**「推理线程数」下拉框按平台 NPU 核数动态生成**（如双核平台为 2/4/6/8/10/12/14，各标注「每核 N 个」），保存时后端自动取整为核数整数倍
  - CAN 卡：波特率 / DBC 上传 / CAN FD / FD 波特率一行配置，通道下拉框标注 FD 能力
  - 网络卡：IP 模式（关闭 / 静态 / DHCP），仅「静态」模式显示 IP / 掩码 / 网关输入框
  - 顶部「导出配置」：一键打包下载整个 config 目录（config.txt / DBC / 标签 / RKNN 模型），便于备份与移植
- `/logs`：文件下载（日志 / 录像双 TAB）

### REST API

| 方法 | 路径 | 认证 | 说明 |
| --- | --- | --- | --- |
| GET | `/api/system` | 无 | 系统监控数据（含 `cpu_cores` / `npu_count` / `npu_cores` / `gpu` / `gpu_freq` 等） |
| GET | `/api/can` | 无 | CAN 接口状态 |
| GET | `/api/can/ifaces` | 无 | 系统 CAN 接口枚举（含 FD 能力） |
| GET | `/api/can/frames` | 无 | 最近原始接收帧 |
| GET | `/api/can/decoded` | 无 | 接收方向 DBC 解码信号 |
| GET | `/api/can/decoded/tx` | 无 | 发送方向 DBC 解码信号 |
| POST | `/api/can/send` | 无 | 注入 CAN/CAN FD 报文 |
| POST | `/api/can/toggle` | 无 | 控制接口 up/down |
| POST | `/api/can/dbc` | 无 | 上传 DBC（热加载） |
| POST | `/api/config` | 无 | 写入配置（`target` 指定模块） |
| GET | `/api/video/caps` | 无 | 摄像头能力 |
| GET | `/api/video/stats` | 无 | 环形队列分阶段计数器 |
| GET | `/api/video/devices` | 无 | 可用视频设备 |
| GET | `/video/mjpeg` | 无 | 原始 MJPEG 流 |
| GET | `/video/mjpeg_ai` | 无 | AI 画框 MJPEG 流 |
| GET | `/api/rec/list` / `GET /recfile/<日期/文件名>` / `GET /api/rec/pack` | 无 | 录像列表 / 单文件 / 打包下载 |
| GET | `/api/logs` / `GET /logfile/<日期/文件名>` / `GET /logs/pack` | 无 | 日志查看 / 下载 / 打包 |
| POST | `/api/rec/start` / `POST /api/rec/stop` | root | 录像控制（默认已自动开启） |
| POST | `/api/reboot` / `POST /api/shutdown` | root | 设备控制 |

## 许可

GPL-3.0-or-later。
