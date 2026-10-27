# rockchip-smart-gateway

An epoll-based solution for CAN / CAN FD acquisition, DBC signal decoding, and TCP/IP communication, with a Web management interface, live video, AI object detection (RKNN + YOLO26), network recording, and a systemd watchdog. Implemented in C11 for Linux edge devices such as the Radxa Rock 4D and Orange Pi 5 series. **Adapts to Rockchip RKNPU platforms**, including RK3588 / RK3576 / RK356x / RV1126 / RV110x / RK3399PRO / RK1808. CPU and NPU core counts, utilization, and available GPU metrics are detected from the running platform.

## Features

- CAN acquisition: classic CAN and CAN FD, multiple interfaces, and automatic discovery of system CAN interfaces.
- DBC decoding: convert received and transmitted CAN frames into physical signal values using DBC databases.
- TCP/IP communication: configurable listening port, network interface binding, and client limit.
- Web management: monitor data, edit configuration, inspect logs, and reboot or shut down the device.
- Live video: V4L2 camera capture, raw and AI-annotated MJPEG, and H.264 fragmented MP4 playback through browser Media Source Extensions (MSE).
- Video frame ring: separate capture / inference / display / encoding cursors, reusable buffer pools, and per-stage drop counters. V4L2 mmap buffers enter the ring by pointer, avoiding a full-frame copy at capture.
- AI detection: RKNN + YOLO26 using the official Ultralytics RKNN single-output format, a parallel inference worker pool, and live detection overlays.
- **Platform-aware NPU scheduling**: choose valid single-core masks from the detected SoC core count, avoiding `rknn_set_core_mask: unavailable core mask`. Workers are distributed across RK3588 three-core, RK3576 two-core, and supported single-core NPUs.
- **Adaptive CPU / NPU displays**: render the actual number of cores reported by the backend, resizing and wrapping indicators as needed.
- **Adaptive GPU monitoring**: use GPU utilization when the driver exposes `load`; otherwise display the current GPU frequency, as on some RK3576 / RK356x drivers.
- Network recording: automatically record AI-annotated video into MP4 (`avc1`) files under `recordings/YYYYMMDD/`, using the configured resolution and H.264 encoder backend. Software encoding uses libx264; hardware encoding uses RKMPP or V4L2 rkvenc when available.
- Shared encoding: recordings and the H.264 live stream consume the same encoder output.
- systemd watchdog: monitor critical thread heartbeats and recover from failures.
- Configuration updates: apply changes through the Web interface without a full service restart where the affected module supports it.
- Operations: date- and size-based log rotation, daily log directories, cleanup scripts, and deployment scripts.
- Data injection and extraction: REST endpoints for CAN transmission, DBC upload and reload, configuration writes, video, raw CAN frames, decoded signals, recordings, and logs.
- English / Chinese interface: English mode covers navigation, forms, dynamic status messages, authentication dialogs, and file controls. Language changes preserve unsaved form values.

## System Architecture

A single process runs multiple threads. The module table `g_modules[]` in `source/core/main.c` registers modules and starts their tasks:

```text
        +--> can_recv_task --> SocketCAN RX --> raw rxq + receive-side DBC decoding
 main --+--> can_send_task --> drain raw txq --> socket + transmit-side DBC decoding
        +--> tcp_task      --> TCP listening, connections, and I/O
        +--> http_task     --> Web management, REST API, and stream dispatch
        +--> video_task    --> V4L2 capture; mmap buffer pointers enter the frame ring
        +--> ai_task       --> event-driven ring consumption --> inference workers
        |                     --> RGB ring buffers + NMS metadata result queue
        +--> rec_task      --> consume displayed NV12 frames --> H.264 --> MP4 (avc1)
        |                     --> shared H.264 live-stream output
        +--> watchdog_task --> heartbeat monitoring + sd_notify
```

| Thread | Responsibilities |
| --- | --- |
| `main` | Initialization, configuration and DBC loading, coordinated shutdown, and watchdog notifications. |
| `can_recv` | Receive CAN frames, append raw frames to rxq, and decode receive-side DBC signals. |
| `can_send` | Drain raw txq into the socket and decode transmit-side DBC signals. |
| `tcp` | TCP listening, connection management, and data transfer. |
| `http` | HTTP management event loop, REST API, and stream connection dispatch. |
| `video` | Capture V4L2 frames into the ring through mmap pointers and advance the produce cursor. |
| `ai` | Consume ring slots through the infer cursor, submit work to the inference pool, and manage models. Each worker has its own RKNN context and is bound by `i % cores`. RGB buffers enter the display path; NMS metadata (names, confidence, and coordinates) enters the composer result queue. |
| `rec` | Consume displayed NV12 frames through the encode cursor, encode H.264, write MP4 files, and publish the same encoded output for live streaming. A sustained AI stall is fatal. |
| `watchdog` | Monitor module heartbeats and issue `sd_notify` notifications. |

The `signal` module only registers signal handlers during `init`; its `task` is empty and does not create a thread.

## Data Flow and Interfaces

### Video

```text
V4L2 camera (MJPEG / YUYV)
  +--> video_task: mmap pointers enter ring --> advance produce cursor
        +--> ai_task: consume infer cursor --> N parallel RKNN workers --> RGB slots
              +--> NMS metadata result queue
              |     +--> composer: latest result + bounding boxes --> display cursor
              |            +--> /video/mjpeg_ai: annotated MJPEG
              |            +--> rec_task: consume NV12 through encode cursor
              |                   +--> H.264 encoder --> MP4 (avc1) recording
              |                         +--> /video/stream: fragmented MP4 for MSE
              +--> raw capture path --> /video/mjpeg (independent of inference)
```

- **Frame ring:** 32 slots with raw / RGB / NV12 / JPEG buffers and monotonically advancing produce / infer / display / encode cursors. Buffers are allocated as needed and returned to stage-specific pools when consumers release them. Ownership is transferred by pointer where supported.
- **Capture:** after V4L2 DQBUF, the mmap buffer pointer enters a ring slot; QBUF is delayed until consumers release it. A full ring increments `capture_dropped`. Capture prefers MJPEG and falls back to YUYV. Resolution comes from `video_width` / `video_height`.
- **Inference:** `ai_task` waits for the produce cursor, pins the raw slot while workers use it, and decodes to RGB. RGB output follows the display path; NMS output contains metadata only, including class names, confidence, and bounding-box coordinates.
- **Display:** the event-driven composer drains its result queue and keeps the latest result, incrementing `render_skipped` for discarded results. It finds the matching RGB slot by sequence, draws overlays, and writes NV12 / JPEG buffers. Each MJPEG connection has an independent streaming thread that copies the latest JPEG; slow clients do not pin frame slots or block other clients.
- **Encoding:** `rec_task` takes ownership of displayed NV12 buffers and returns them to the pool after encoding. Frames with a resolution different from the encoder are skipped and counted in `encode_skipped`.
- **Mandatory AI:** missing models or an unavailable NPU cause startup failure. The annotated stream does not substitute raw frames when AI is unavailable. A running AI pipeline that produces no new annotated frame for more than one second sets a fatal condition and exits with status 1; systemd `Restart=on-failure` restarts the service.
- **H.264 fan-out:** the live stream reuses recording encoder output. A client joins at an IDR frame and receives an `ftyp` / `moov` initialization segment followed by `moof` / `mdat` fragments. Recording-session changes require a new initialization segment and reconnect the client.

| Endpoint | Description |
| --- | --- |
| `GET /video/stream` | H.264 fragmented MP4 live stream for browser MSE. An active encoded session and AI output are required; an unavailable session returns 503. |
| `GET /video/mjpeg` | Raw MJPEG stream for a browser `<img src>` or ffplay. |
| `GET /video/mjpeg_ai` | MJPEG stream with live AI detection overlays. |
| `GET /api/video/caps` | Camera formats, resolutions, and frame-rate capabilities. |
| `GET /api/video/codeccaps` | Encoder backend resolution and frame-rate capabilities. |
| `GET /api/video/stats` | Frame-ring sequences, per-stage drop counters, and buffer-pool usage. |
| `GET /api/video/devices` | Available video devices. |

### Browser Playback and Stall Diagnostics

The dashboard uses H.264 fMP4 / MSE when supported, with MJPEG as the compatibility path. Playback aims to keep up with live video while avoiding unnecessary reconnects; actual delay depends on capture, inference, encoding, transport, and browser decoding.

- The fMP4 writer advances a continuous decode timeline using the previous sample's duration. Fragment decode times and sample durations stay consistent, avoiding gaps between browser buffer ranges.
- The browser serializes SourceBuffer mutations. Each append or eviction waits for `updateend` before the next operation, so later network chunks cannot overtake earlier MP4 bytes.
- Session generations prevent callbacks from an old aborted stream from stopping a replacement stream. Errors and transport stalls trigger controlled reconnection.
- Already played data is periodically evicted from the MSE buffer. Ordinary playback lag is corrected within the available buffer instead of immediately reconnecting to another GOP.
- At connection time, the server sends the historical GOP up to the newest encoded frame immediately, in decode order. Live pacing is anchored to that newest frame, rather than replaying the old GOP in real time and permanently retaining its age as delay.
- In Chrome / Chromium, the player targets approximately 40 ms of buffered media and catches backlog above 120 ms. The compatibility profile retains a 200 ms target. These are playback buffer settings, not guaranteed end-to-end latency. Both paths preserve the original frame-to-AI-result association.
- The MJPEG path uses one fetch connection for both receipt counting and display. It retains only the latest pending frame while image decoding is busy, and keeps incomplete JPEG frames across network chunks.
- The FPS badge reports displayed frames when browser playback-quality counters are available; its tooltip distinguishes displayed FPS from received FPS. Receipt counts alone do not prove that the browser is displaying smoothly.

The status line below the video shows received FPS, buffer lead, estimated total delay, and decoder frame drops, without developer tools. The total estimate combines the device's capture-to-encoder age and the estimated encoder-to-browser presentation delay. A valid monotonic V4L2 capture timestamp is preferred; otherwise the device uses capture dequeue time. Exposure, timestamp accuracy, network jitter, and compositor timing limit this estimate, so a 100 ms target is not a verified camera-to-screen guarantee.

Chrome's `requestVideoFrameCallback` supplies the presented frame's media time and decoder processing time when available. The lower-level `estimatedStreamDelayMs` remains an encoder-output-to-presentation estimate. See the [frame callback specification](https://wicg.github.io/video-rvfc/) for the presentation metadata semantics.

The line also shows the frame rate actually negotiated with the camera and the latest capture-to-render processing age. Requested FPS can exceed camera capability: a device may accept a 60 FPS request but negotiate 30 FPS. Decoder drops on the status line are a per-second rate; cumulative decoder counts remain available in diagnostics. Parallel inference results are coalesced by their highest capture sequence, so an older worker finishing last cannot replace a newer result.

For additional details, read the current browser-side state in the browser developer console:

```javascript
window.getGatewayVideoDiagnostics()
```

| Diagnostic | Meaning |
| --- | --- |
| `transport` | Current transport: `H.264/MSE`, `MJPEG`, or `idle`. |
| `receivedFps` / `displayedFps` | Received stream rate and displayed-frame rate. MSE receipt counting uses fragment signatures as an approximation. |
| `bufferAheadSeconds` | Buffered media ahead of the current playback position; this is a playback-buffer measure, not a measurement of end-to-end camera latency. |
| `estimatedStreamDelayMs` / `estimatedTotalDelayMs` | Approximate encoder-output-to-presentation delay and total delay including device processing, or `null` without timing headers. |
| `playbackProfile` / `targetBufferMs` / `browserDecodeMs` | Active browser profile, buffer target, and decoder processing time when frame callbacks provide it. |
| `paused` / `seeking` / `currentTime` / `buffered` | Playback state, position, and available media ranges. |
| `playbackRate` / `decoderError` | Current catch-up rate and any HTML media error code. |
| `readyState` | The video element's HTML media readiness state. |
| `totalVideoFrames` / `droppedVideoFrames` | Browser playback-quality counters, when supported. |
| `starts` / `reconnects` / `catchups` | MSE session starts, error-driven reconnects, and in-buffer catch-up operations. |
| `appendErrors` / `lastError` | SourceBuffer errors and the latest recorded MSE error. |
| `cameraFps` / `processingAgeMs` | Negotiated camera rate and latest capture-to-render processing age from the device. |
| `droppedFramesPerSecond` / `sourceDroppedFrames` | Current decoder-drop rate and cumulative capture / driver / inference failures; intentional latest-frame replacement is counted separately by the device. |

Compare these values with `GET /api/video/stats` over the same time interval. Growing capture or render drop counts point to an upstream stage; a healthy received rate with low displayed FPS or increasing browser drop counts points to playback work. Buffer growth with adequate incoming data indicates that playback is falling behind. Use service logs to correlate AI stalls, encoder errors, or session rotation.

Device video statistics include `capture_fps`, `display_seq`, `displayed_frames`, `encoded_frames`, `encode_replaced`, and current / maximum `processing_age_ms`. Compare counter differences over time; cumulative counts alone do not describe current playback. `render_skipped` and `encode_replaced` include intentional latest-frame selection and should not be confused with corrupt frames or camera-driver loss.

### CAN

```text
RX: SocketCAN frame --> can_recv_task (nonblocking epoll)
       +--> raw rxq for application consumers
       +--> http_can_record_rx --> /api/can/frames (latest 32 frames)
       +--> receive-side DBC decoding --> /api/can/decoded

TX: can_tx_frame() --> txq --> eventfd wakes can_send_task --> drain into socket
       +--> transmit-side DBC decoding --> /api/can/decoded/tx
       +--> EAGAIN: requeue; a full queue increments the drop count
```

- Each interface has independent rxq / txq queues of raw `canfd_frame` values. DBC decoding and the raw-frame cache are independent.
- Read failures rebuild the socket and reapply filters. A disconnected interface does not block the entire application from starting.

Data injection endpoints:

| Endpoint | Description |
| --- | --- |
| `POST /api/can/send` | Transmit one CAN / CAN FD frame. Example body: `ifname=can0&id=0x123&data=01 02 03`. Accepts a `0x` prefix and space, comma, or hyphen separators. |
| `POST /api/can/toggle` | Set an interface up or down. Example body: `ifname=can0&action=up`. |
| `POST /api/can/dbc` | Upload DBC text with `?ifname=can0`; validate, reload, and save it. |
| `POST /api/config` | Apply configuration for `target=can`, `net`, `ip`, `video`, or `ai`, restarting the affected module where needed. An omitted target applies all supported groups. |

Data extraction endpoints:

| Endpoint | Description |
| --- | --- |
| `GET /api/can` | CAN interface state, bitrate, and FD status. |
| `GET /api/can/ifaces` | Enumerate system CAN interfaces and FD support. |
| `GET /api/can/frames` | Recent raw received frames: ID, length, data, and flags. |
| `GET /api/can/decoded` | Recently decoded receive-side signals. |
| `GET /api/can/decoded/tx` | Recently decoded transmit-side signals. |

### TCP

```text
Listen: tcp_task --> configured tcp_port, optionally bound to tcp_bind
          +--> accept client --> epoll registration and per-client I/O state
Receive: recv client data --> currently discarded; not injected into CAN
Transmit: tcp_tx_packet() --> txq --> eventfd wakes tcp_task --> client socket
            +--> selected client_idx or broadcast when client_idx < 0
            +--> per-client wbuf retains unsent bytes for slow clients
```

- The external TCP server uses nonblocking sockets and epoll. `max_clients` defaults to 16; a per-client write buffer handles partial writes.
- The TX queue, eventfd notification, and targeted / broadcast dispatch are available for application integration, such as CAN-to-TCP forwarding. The current TCP receive path does not forward data to the CAN bus.

### System Monitoring

`GET /api/system` returns live CPU, memory, disk, temperature, network, NPU, and GPU data:

| Field | Description |
| --- | --- |
| `cpu_cores` | Per-core cumulative CPU accounting values; the array length is the actual core count, and the dashboard calculates utilization from successive samples. |
| `npu_count` / `npu_cores` | NPU core count and per-core load in percent; -1 indicates an unavailable metric. Core information is derived from kernel `debugfs` data for three-core, two-core, and single-core platforms. |
| `gpu` | GPU utilization in percent when the driver exposes `load`; otherwise -1. |
| `gpu_freq` | Current GPU frequency in MHz, used as the displayed metric when utilization is unavailable. |

## Rockchip RKNPU Platform Adaptation

NPU scheduling and hardware displays adapt to the actual platform:

- **NPU masks:** at startup, `source/yolo/rknn_yolo.c` reads `/proc/device-tree/compatible`. RK3588 / RK3588S use three cores, RK3576 / RK3576M use two, and supported RK356x / RV1126 / RV110x / RK3399PRO / RK1808 devices use one. Unknown platforms fall back to one core. Workers use `1u << (idx % cores)` to select legal single-core masks and distribute work.
- **Inference workers:** after eight seconds of warmup, the controller compares core-balanced worker counts using four-second windows of actual rendered/encoded throughput and frame age. A worse trial returns to the measured best count. After selection, a two-minute minimum hold prevents repeated probing during ordinary load variation; routine reassessment has a three-minute cooldown. A small reserve pool avoids model reloads when demand changes. Inference has no additional sleep interval, and every displayed frame retains its own AI result.
- **CPU / NPU indicators:** the dashboard uses `cpu_cores.length` / `npu_count` to build and size core indicators dynamically, wrapping them when needed.
- **GPU metrics:** use `gpu` when available and `gpu_freq` otherwise, as described above.
- **Model selection:** before compilation, `build.sh` validates or converts `config/yolo26.rknn` for the platform detected through `/proc/device-tree/compatible`, or explicitly selected with `-P`.

## Directory Layout

```text
rockchip-smart-gateway/
├── CMakeLists.txt
├── README.md
├── rockchip-smart-gateway.service  # systemd service unit
├── include/                       # headers
│   ├── core/
│   │   ├── common.h               # application context and shared utilities
│   │   ├── config.h               # configuration structures
│   │   ├── log.h                  # logging API
│   │   ├── ring.h                 # lock-free queue
│   │   ├── epoll_util.h           # epoll helpers
│   │   └── version.h              # generated version macros; gitignored
│   ├── can/
│   │   ├── can_queue.h            # CAN RX / TX queues
│   │   ├── can_socket.h           # SocketCAN API
│   │   └── dbc_parser.h           # DBC parser API
│   ├── tcp/
│   │   ├── tcp_server.h           # TCP server API
│   │   └── tcp_queue.h            # TCP TX queue
│   ├── http/
│   │   ├── http.h                 # HTTP server API
│   │   ├── http_internal.h        # internal definitions
│   │   ├── http_util.h            # HTTP form / URL helpers
│   │   ├── http_api_can.h         # CAN API declarations
│   │   └── http_api_dbc.h         # DBC API declarations
│   ├── video/
│   │   ├── video_stream.h         # V4L2 streaming API
│   │   ├── frame_ring.h           # four-cursor frame ring and stage counters
│   │   ├── video_rec.h            # recording API
│   │   ├── h264_encoder.h         # software / Rockchip H.264 encoder wrapper
│   │   ├── h264_stream.h          # shared H.264 fan-out and fMP4 stream API
│   │   └── rec_mp4.h              # MP4 (avc1) muxer API
│   ├── yolo/
│   │   ├── rknn_yolo.h            # RKNN YOLO entry point and npu_core_count
│   │   ├── yolo_types.h           # detection result structures
│   │   ├── yolo_image.h           # JPEG / YUYV / letterbox processing
│   │   ├── yolo_postprocess.h     # single-output model postprocessing
│   │   └── yolo_draw.h            # detection overlay API
│   └── watchdog/
│       └── watchdog.h             # watchdog API
├── source/                        # source code
│   ├── core/      main.c / config.c / log.c
│   ├── can/       can_queue.c / can_socket.c / dbc_parser.c
│   ├── tcp/       tcp_server.c / tcp_queue.c
│   ├── http/      http.c / http_util.c / http_api_*.c
│   │              # can / dbc / config / network / system / video / rec / ai /
│   │              # logs / reboot / cpufreq / nettest
│   ├── video/     video_stream.c / frame_ring.c / video_rec.c / h264_encoder.c /
│   │              # h264_stream.c / rec_mp4.c
│   ├── yolo/        rknn_yolo.c / yolo_image.c / yolo_postprocess.c / yolo_draw.cpp
│   └── watchdog/  watchdog.c
├── tests/        frame_ring_test.c
├── html/         index.html / config.html / dbc.html / logs.html + css/ + js/
├── config/       runtime configuration, RKNN model, labels, and DBC templates
│                 # config.txt is device-specific and gitignored
├── docs/         documentation and screenshots
├── scripts/      build.sh / deploy.sh
├── bin/          generated executable; gitignored
├── build/        generated CMake build directory; gitignored
├── logs/         runtime logs; gitignored
├── recordings/   runtime recording output; gitignored
└── .gitignore
```

## Requirements

- gcc / g++ with C11 / C++11 support. Detection overlay rendering uses C++ and OpenCV.
- CMake >= 3.10.
- libsystemd.
- libnl-3 / libnl-route-3.
- librknnrt for the board's RKNN runtime. Models are converted with rknn-toolkit2 on a supported board or PC.
- libjpeg for JPEG decoding and AI overlays.
- OpenCV 4 core + imgproc for bounding boxes and label text.
- Linux SocketCAN support.
- An available H.264 encoder backend: FFmpeg libx264 for software encoding, or a compatible Rockchip hardware backend. The V4L2 M2M rkvenc path uses an encoder device such as `/dev/video-enc0`; the optional FFmpeg backend can provide `h264_rkmpp`.

Example dependency installation:

```bash
apt install build-essential cmake libsystemd-dev libnl-3-dev libnl-route-3-dev libjpeg62-turbo-dev libopencv-dev
```

## Build

```bash
cd /home/qianruo/rockchip-smart-gateway
./scripts/build.sh            # Release by default; detect the board's RKNPU platform
./scripts/build.sh -D         # Debug
./scripts/build.sh -P rk3576  # Explicit target platform, for example for cross-deployment
./scripts/build.sh -C         # Remove build output and logs
```

`build.sh` checks the toolchain, development libraries, and librknnrt before building. Missing dependencies are reported with installation commands, and the build stops.

Platform detection and model synchronization:

- The default platform is derived from `/proc/device-tree/compatible`, for example `rockchip,rk3576` -> `rk3576`. Use `-P` to override it.
- Conversion, default runtime loading, and model uploads all use `config/yolo26.rknn`. Old `config/models/<platform>/yolo26.rknn` configuration entries migrate to this path; duplicate platform models are no longer required.
- The build scans `config/` recursively for `.pt` weights. One discovered input is automatically converted into `config/yolo26.rknn`, even if an older RKNN file already exists. Multiple inputs stop the build with a candidate list; use `YOLO_WEIGHTS` to select one explicitly. With no PT input, reuse the valid RKNN model; if neither is available, the original default-weight download/export workflow applies. Conversion invokes the official Ultralytics CLI, `yolo export model=<weights> format=rknn name=<platform>`. Use `./scripts/build.sh -M` to explicitly reconvert an existing model. An explicit weight path must exist. After a successful build, the selected project-owned PT input and cached conversion copies are removed. Failed builds retain them; external user-supplied input files are preserved. Weights are not deployed.
- `scripts/model_sync.py` compares source, deployed, and last-deployed SHA-256 values. A model uploaded on the device is pulled back when the source is unchanged; a newly converted source model is deployed when the device is unchanged. Independent changes on both sides stop synchronization instead of overwriting either model. The shared baseline under `config/models/` is gitignored.

Build output: `./bin/rockchip-smart-gateway`.

At configuration time, CMake generates `include/core/version.h` with `APP_NAME`, `APP_VERSION`, `APP_GIT_COMMIT`, `APP_GIT_BRANCH`, `APP_GIT_DIRTY`, `APP_BUILD_TYPE`, and `APP_BUILD_DATE`. This generated file is listed in `.gitignore`.

## Run

```bash
cd /home/qianruo/rockchip-smart-gateway
./bin/rockchip-smart-gateway
```

Run from the project root so that `config/`, `html/`, and `logs/` are available.

## systemd Deployment

First build the executable with `./scripts/build.sh`.

```bash
sudo ./scripts/deploy.sh        # Install and start; the default action, equivalent to -i
sudo ./scripts/deploy.sh -i     # Install and start
sudo ./scripts/deploy.sh -u     # Uninstall: stop service and remove unit and installation directory
sudo ./scripts/deploy.sh -r     # Reinstall: uninstall, then install
sudo ./scripts/deploy.sh -h     # Show help
```

`deploy.sh` requires root privileges and installs into `/opt/rockchip-smart-gateway/`:

| Source | Destination |
| --- | --- |
| `bin/<executable>` | `/opt/.../bin/` |
| `html/` | `/opt/.../html/` |
| Runtime `config/` files and the selected platform model | `/opt/.../config/`; preserve Web configuration and uploaded models, excluding conversion weights and synchronization bookkeeping. |
| systemd service unit | `/etc/systemd/system/` |

`config/config.txt` is device-specific runtime configuration, including the camera device path. It is gitignored, as is the generated `include/core/version.h`, so normal configuration and build activity do not add these files to version control.

Service name: `rockchip-smart-gateway`.

Inspect service state and logs:

```bash
systemctl status rockchip-smart-gateway
journalctl -u rockchip-smart-gateway -f
```

Service settings include:

- `Type=notify`.
- `WatchdogSec=10`.
- `Restart=on-failure` with a five-second restart delay.
- `AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW`.
- `ProtectSystem=strict`, with explicit writable installation paths for `config/`, `logs/`, and `recordings/`; `html/` remains read-only.

## Configuration

Web configuration changes are saved to `config/config.txt` and applied to the running application.

| Key | Format | Description |
| --- | --- | --- |
| `can_ifname` | `<name>` | CAN interface name; repeatable. When no configuration file exists, discover the system's actual CAN interfaces. |
| `can_bitrate` | `<name> <bps>` | Bitrate; default 500000. |
| `can_dbitrate` | `<name> <bps>` | CAN FD data bitrate; default 2000000. |
| `can_fd` | `<name> on\|off` | Enable or disable CAN FD. |
| `can_up` | `<name> on\|off` | Bring the interface up at startup. |
| `can_filter` | `<name> <id> <mask>` | Receive filter with hexadecimal ID / mask; repeatable per interface. |
| `tcp_port` | `<port>` | TCP port; default 6666. |
| `max_clients` | `<n>` | Maximum TCP clients; default 16. |
| `tcp_bind` | `<ifname>` | Bind TCP listening to this interface; empty means all interfaces (`INADDR_ANY`). |
| `ip_ifname` | `<ifname>` | Interface whose IP configuration is managed; defaults to the selected TCP interface when applicable. |
| `ip_mode` | `static` / `dhcp` / empty | Static address, DHCP, or unmanaged mode. |
| `ip_addr` / `ip_mask` / `ip_gw` | `<IPv4-address>` | Static IPv4 address, subnet mask, and optional gateway. |
| `video_device` | `<path>` | Video device; default `/dev/video0`. |
| `video_width` / `video_height` | `<n>` | Resolution; default 640 x 480. |
| `video_fps` | `<n>` | Requested frame rate, 1-120, applied using V4L2 S_PARM; 0 uses the driver default. |
| `video_codec` | `software` / `hardware` | H.264 encoder backend: libx264 or supported Rockchip hardware. The default is `software`; backend availability determines whether fallback is possible. |
| `video_decoder` | `software` | Camera image decoding uses libjpeg-turbo/YUYV conversion. Hardware decoding is unavailable in this build and is rejected by the configuration API. Older configuration files default to software decoding. |
| `can_dbc` | `<name> <path>` | Per-interface DBC path; empty disables decoding for that interface. |
| `ai_model` | `<path>` | Official YOLO26 single-output RKNN model; default `config/yolo26.rknn`. AI is mandatory; missing models or NPU initialization failure abort startup. |
| `ai_names` | `<path>` | Class labels, one class name per line in COCO format; default `config/coco.names`. |
| `ai_input_size` | `<n>` | Model input side length, 32-2048; default 640. |
| `ai_conf` | `<n>` | Confidence threshold as an integer percentage, 1-100; default 25 (0.25). |
| `ai_nms` | `<n>` | NMS IoU threshold as an integer percentage, 1-100; default 45 (0.45). |
| `ai_interval_ms` / `ai_threads` | Legacy | Older files may retain these keys. The runtime uses adaptive worker selection and camera-paced inference instead; the Web UI no longer exposes manual controls. |

Without a configuration file, defaults are applied to CAN interfaces discovered through netlink with `kind=="can"`. If no CAN interfaces are discovered, the application can run with zero CAN channels; it does not invent unavailable interfaces.

### DBC Templates

`config/` contains two CAN channel templates. Enable them with `can_dbc <interface> <path>`, or upload a DBC file through the Web configuration page:

- `config/can0.dbc`: powertrain / battery examples, including `EngineData` and `BatteryStatus`.
- `config/can1.dbc`: chassis / position examples, including a `Status` message with Motorola big-endian signals and an extended-ID `VehiclePos` message.

The parser recognizes `BO_` message lines and `SG_` signal lines. Other lines, including `//` comments, are ignored. IDs are decimal; extended-frame IDs set bit 31 on the 29-bit ID, for example `0x18FF50E5` -> `2566869221`.

## Web Management

The HTTP server listens on port 80 by default:

- `/`: dashboard, including live video without a seek bar, a fullscreen button, displayed FPS, CPU utilization and per-core indicators, per-core NPU load, GPU load or frequency, memory, disk, and network rates. There are no manual focus controls.
- `/dbc`: receive-side and transmit-side DBC signal views.
- `/config`: runtime configuration for camera device, resolution, frame rate and encoder, AI confidence/NMS and model uploads, CAN settings and DBC upload, network addressing, CPU frequency controls, network tests, and configuration export. Camera frame-rate choices intersect capture and encoder capabilities. Use each card's Save button to apply its settings.
- `/logs`: file downloads with separate Logs, Recordings, and Detections tabs. Detections lists one Excel workbook per day and provides authenticated Download/Delete controls.

The camera card combines resolution and frame rate in one selector, places the decoder below the encoder, and keeps Save in its title bar. Continuous autofocus is enabled when capture starts and restored if it is externally disabled. Dynamic frame intervals are disabled where supported to request a fixed cadence; actual delivery still depends on the camera firmware, lighting, and negotiated mode. The dashboard and configuration page have no manual lens controls. Each captured frame carries a lens-position sample and its monotonic sample time. These controls are read immediately after dequeue, rather than being queried later when a file is saved; this is not exposure-synchronized lens telemetry.

CPU affinity uses detected kernel CPU capacity, with hardware maximum frequency as a fallback, within the process CPU allowance. Heavy video capture, inference, composition and encoding threads use the higher-capacity group; main/control/network/watchdog and background report tasks use the lower-capacity group. Export and compression inherit the lower-capacity mask and run at reduced priority. Homogeneous or unidentifiable topologies retain all allowed CPUs, without fixed core numbers or platform-specific CPU-part tables.

The CPU frequency card enumerates kernel policy groups and their related CPUs, governors, and hardware frequency tables. Each group is configured independently. Requested limits are shown separately from effective limits and current frequency, so thermal throttling is visible. Saving validates the entire request before writes, checks results, and persists device settings in `config/cpu_freq.conf`; startup restores saved requests. Deployment preserves that device file. Kernel thermal and hardware protections remain active.

Every completed inference records metadata before display coalescing can discard a result. Files → Detections exposes one standard `detections_YYYYMMDD.xlsx` workbook per day. Durable append data is kept in a hidden daily `.rows` file; downloads and archive requests export a consistent snapshot to XLSX without repeatedly rewriting a large workbook on the video path. Photos and per-frame JSON are not generated. Every detected object contributes one spreadsheet row with capture time (milliseconds and timezone), image width/height, target name, confidence, x1/y1/x2/y2 coordinates, frame sequence, raw lens position, millimeter focal length, and autofocus state. A frame with no detections contributes a `none` row with null confidence/coordinates. Coordinates are the inference result for that frame, before display smoothing. The first row contains English column names. Numeric and boolean values retain spreadsheet types; unavailable values use blank cells. Workbooks automatically add worksheets when the Excel row limit is reached. The append format escapes tabs, newlines, and control characters in target names. A bounded 512-entry background queue waits for space rather than dropping inference records; buffered records flush about once per second or at 64 KiB, and shutdown drains accepted records. Failed appends truncate back to the prior complete report size and retain the batch for retry. During shutdown or model reload, persistent storage failure has a bounded retry limit and explicitly logs unsaved data so it cannot deadlock inference worker shutdown. Millimeter focal length is `null`: V4L2 defines no physical unit for `FOCUS_ABSOLUTE`, and the connected UGREEN camera reports zero in its UVC focal-length descriptors. See the [Linux camera control reference](https://docs.kernel.org/userspace-api/media/v4l/ext-ctrls-camera.html).

Daily Excel workbooks use the same Download/Delete controls as recordings. The Detections tab also provides Pack download and Clear detection results in the top bar; the table loads automatically on selection and refreshes every five seconds without a Query button. Archives contain Excel workbooks only. Clear and single-day deletion also remove the corresponding append data and reject pre-deletion queued batches, while later inference frames can create a fresh report. Deletion requires administrator authentication and confirmation. Deleting today's file removes its saved history; subsequent frames create a new report for the same date. Append and delete operations are serialized independently of the video task. Legacy per-photo JSON can be backfilled with `python3 scripts/backfill_detection_reports.py --root <detection-directory>` while the service is stopped; `--dry-run` previews the operation and `--self-test` checks the migration logic.

In **Files → Recordings**, **Split recording** saves the active MP4 and continues into a new file at the next keyframe, normally within one GOP. The encoder and live-stream connection remain active. The list refreshes after completion; the saved segment becomes downloadable and the new segment remains marked Recording. Downloads of the unfinished active MP4 are disabled until it is saved. The split operation requires administrator access and rejects stale filenames or duplicate pending requests.

AI is required and has no disable switch. Inference concurrency is automatic; confidence/NMS and model uploads remain configurable. The CAN card displays bitrate, DBC, CAN FD, and FD bitrate controls, with FD capability shown in the channel selector. Static IP fields are visible only in static mode. Export Config packages the config directory, including configuration, DBC files, class labels, and the RKNN model.

The language button switches between English and Chinese; in English mode it is labeled `ZH` and in Chinese mode it is labeled `EN`. Theme switching is independent of language.

### Authentication

The server uses HTTP Basic authentication against local Linux account credentials:

- **User:** an existing local account with a valid password. Required for the dashboard (`/` and `/index.html`) and DBC pages (`/dbc` and `/dbc.html`).
- **Administrator:** a valid local `root` account, or a valid local account belonging to the `sudo` group. Required for the configuration page, Logs page, protected downloads, and management writes listed below. A board with a locked root password can use its existing sudo-group administrator account.
- **None:** the handler does not enforce authentication. This applies to the read-only API and video routes marked below; authenticating the dashboard page does not make all related API routes private.

The frontend prompts for administrator credentials for protected requests and keeps its Basic credential value in session storage for the current browser session. Server-side authentication still checks every protected request.

### REST API

Authentication below follows the route definitions in `source/http/http.c`.

| Method | Path | Authentication | Description |
| --- | --- | --- | --- |
| GET | `/api/system` | None | System monitoring, including `cpu_cores`, `npu_count`, `npu_cores`, `gpu`, and `gpu_freq`. |
| GET | `/api/can` | None | CAN interface state. |
| GET | `/api/can/ifaces` | None | CAN interface discovery and FD capabilities. |
| GET | `/api/can/frames` | None | Recent raw received CAN frames. |
| GET | `/api/can/decoded` | None | Receive-side decoded signals. |
| GET | `/api/can/decoded/tx` | None | Transmit-side decoded signals. |
| POST | `/api/can/send` | Administrator | Transmit a CAN / CAN FD frame. |
| POST | `/api/can/toggle` | Administrator | Set a CAN interface up or down. |
| POST | `/api/can/dbc` | Administrator | Upload and reload a DBC file. |
| GET | `/api/config` | None | Read runtime configuration. |
| POST | `/api/config` | Administrator | Write configuration for the selected `target`. |
| GET | `/api/config/export` | Administrator | Download a config directory archive. |
| POST | `/api/ai/upload` | Administrator | Upload an RKNN model or class labels. |
| GET | `/api/network` | None | Network state. |
| GET | `/api/network/ifaces` | None | Available network interfaces. |
| POST | `/api/nettest` | Administrator | Start an asynchronous ping or download-speed test. |
| GET | `/api/nettest/result?id=<id>` | None | Read an asynchronous test's status and result. |
| GET | `/api/cpu_freq` | None | CPU frequency, available frequencies, and governors. |
| POST | `/api/cpu_freq` | Administrator | Update CPU governor and frequency limits. |
| GET | `/api/video/caps` | None | Camera capabilities. |
| GET | `/api/video/codeccaps` | None | Encoder resolution and frame-rate capabilities. |
| GET | `/api/video/stats` | None | Frame-ring per-stage counters. |
| GET | `/api/video/devices` | None | Available video devices. |
| GET | `/api/camera` | None | Current lens controls, limits, and hardware encoder availability. |
| GET | `/api/detections` | Administrator | Latest 200 completed detection photo/JSON pairs. |
| GET | `/api/detections/reports` | Administrator | Daily detection Excel reports, newest day first. |
| GET | `/api/detections/reports/pack` | Administrator | Export and download daily Excel workbooks as a tar.gz archive. |
| POST | `/api/detections/reports/clear` | Administrator | Clear daily workbooks and their append data; subsequent inference continues saving. |
| POST | `/api/detections/reports/delete` | Administrator | Delete a daily Excel workbook; body is its relative date/filename. New frames can recreate today's report. |
| GET | `/detectionfile/<date/filename>` | Administrator | Download a daily Excel workbook; legacy pair downloads remain compatible when the files exist. Unsafe paths and symlinks are rejected. |
| GET | `/video/stream` | None | H.264 fragmented MP4 live stream for MSE. |
| GET | `/video/mjpeg` | None | Raw MJPEG stream. |
| GET | `/video/mjpeg_ai` | None | AI-annotated MJPEG stream. |
| GET | `/api/rec/status` | None | Recording state and current file. |
| GET | `/api/rec/list` | None | Recording list. |
| GET | `/recfile/<date/filename>` | Administrator | Download an individual recording. |
| GET | `/api/rec/pack` | Administrator | Download a recording archive. |
| POST | `/api/rec/start` / `/api/rec/stop` | Administrator | Recording control; automatic recording is enabled by default. |
| POST | `/api/rec/split` | Administrator | Queue a keyframe-aligned file split; body is the current relative recording filename. Continues with the same encoder and stream session. |
| POST | `/api/rec/delete` | Administrator | Delete a recording; the active recording is protected. |
| POST | `/api/rec/clear` | Administrator | Clear recordings while retaining the active file. |
| GET | `/api/logs` | Administrator | List log files. |
| GET | `/logfile/<date/filename>` | Administrator | Download an individual log. |
| DELETE | `/logfile/<date/filename>` | Administrator | Delete a log. |
| GET | `/logs/pack` | Administrator | Download a log archive. |
| POST | `/api/logs/clear` | Administrator | Clear log files. |
| GET or POST | `/api/reboot` / `/api/shutdown` | Administrator | Reboot or shut down the device. |

## License

GPL-3.0-or-later.
