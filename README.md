# Autonomous Driving Car

A Linux vehicle application combining V4L2 camera capture, Rockchip RKNN YOLO26 detection, differential target following, SocketCAN motor integration, and a browser management interface. The backend uses C11, with C++11/OpenCV for drawing overlays; the frontend uses plain HTML, CSS, and JavaScript.

The current hardware validation platform is **Radxa Rock 4D / RK3576**. The implemented control path uses conventional PD/PI controllers and real wheel feedback. An LLM reasoning layer is a planned extension; no LLM provider, model invocation, or reasoning-to-control integration exists.

## Implementation status

| Area | Implemented behavior | Boundary |
| --- | --- | --- |
| Perception | YOLO26 inference, original-image coordinates, frame-matched overlays, adaptive worker count | Requires a compatible RKNN model and working NPU runtime |
| Following | Target selection, automatic/manual speed strategies, steering PD, independent wheel PI state | Actual motion requires a validated motor profile, fresh feedback, CAN, and successful FIFO scheduling |
| Motor communication | Commands and feedback share the public CAN module | No production motor profile or drive-specific startup handshake is supplied |
| Video and records | H.264/MSE, MJPEG, annotated MP4, daily detection TXT | Display/encoding may skip old frames; no zero-drop or 100 ms end-to-end guarantee |
| Web management | Monitoring, camera/model configuration, network IP, CPU policies, CAN/DBC, file management | HTTP uses fixed port 80 |
| VOFA+ | Configurable TCP server, four JustFloat channels | Actual-value channels need valid measured wheel feedback |
| Real-time platform | Controller requests `SCHED_FIFO` priority 20 | FIFO admission and PREEMPT_RT kernel support are separate checks |

The supplied configuration does not enable a real motor. Following starts stopped after every service restart. Platform mappings for other Rockchip devices do not constitute hardware compatibility validation.

## Architecture and code map

The executable is one process. [main.c](source/core/main.c) constructs a shared `app_ctx_t`, initializes modules, starts workers, and coordinates shutdown. Modules own their lifecycle functions and use mutexes, atomics, bounded queues, and condition variables to exchange data.

```text
V4L2 camera
    -> frame ring: raw -> RGB -> annotated NV12/JPEG
                       |             |
                 RKNN workers        +-> MJPEG / H.264 -> browser
                       |                             +-> MP4 recording
                       +-> completed detections
                              +-> daily TXT writer
                              +-> following controller -> shared CAN -> motor drives
                                         ^                    |
                                         +-- measured wheels -+
                                         |
                                  telemetry snapshot
                                         -> TCP event loop -> VOFA+ client

HTTP :80 -> pages, configuration APIs, status, and protected file operations
```

| Directory / entry | Responsibility |
| --- | --- |
| `source/core/`, `include/core/` | Entry point, configuration persistence, logging, CPU affinity, shared context |
| `source/video/`, `include/video/` | V4L2 capture, frame ring and buffer ownership, H.264 encoding/streaming, MP4 recording |
| `source/yolo/`, `include/yolo/` | RKNN lifecycle, adaptive inference pool, image conversion, postprocessing, overlays, TXT writer |
| `source/control/car_sport_ctrl.c` | Target association, speed/steering control, wheel PI, motion interlocks, following API |
| `source/can/`, `include/can/` | Shared SocketCAN transport, CAN enable/disable, filters, DBC parsing, motor profile mapping |
| `source/tcp/`, `include/tcp/` | VOFA TCP listener/client lifecycle and event loop; system interface IP helpers |
| `source/telemetry/`, `include/telemetry/` | Read-only controller sampling, JustFloat encoding, VOFA configuration/API |
| `source/http/`, `include/http/` | HTTP reactor, authentication, configuration, monitoring, video/file APIs |
| `source/watchdog/`, `include/watchdog/` | Worker heartbeat supervision and systemd notification |
| `html/` | Browser pages, shared themes, bilingual labels, configuration and player logic |
| `scripts/` | Build/deploy, model synchronization, historical report migration and maintenance utilities |
| `tests/` | C/C++ and Python regression checks; an additional JavaScript player test is separate from CTest |

Main workers include CAN receive/send, TCP, HTTP, capture, YOLO dispatch, recording/encoding, and watchdog. YOLO owns inference workers and a rendering composer; control owns a joinable event-driven worker; detection reports have a writer thread. **VOFA has no separate network thread**: sampling and socket processing run in `tcp_task`.

Capture, inference, and recording work use the detected higher-capacity CPU group. Network/control/background workers use the lower-capacity group. Unknown or homogeneous CPU layouts retain the originally allowed CPU set.

## Requirements

Use Linux with V4L2 and the Rockchip NPU driver/runtime. SocketCAN and a compatible interface are needed for CAN functions. The build requires GCC/G++ supporting C11/C++11, CMake 3.10+, Python 3, libsystemd, libnl-3/libnl-route-3, libjpeg, OpenCV core/imgproc, and board-compatible `librknnrt.so`.

On Debian/Ubuntu, development packages include:

```bash
sudo apt install build-essential cmake python3 pkg-config \
  libsystemd-dev libnl-3-dev libnl-route-3-dev \
  libjpeg62-turbo-dev libopencv-dev libavcodec-dev libavutil-dev
```

Install `librknnrt.so` from the SDK appropriate to the board and kernel. FFmpeg libraries are optional at build time: software H.264 requires the `libx264` encoder; hardware encoding uses an available `h264_rkmpp` or V4L2 encoder. Runtime capability detection determines which choices can be used.

YOLO is a required startup component. Missing/incompatible models or unavailable NPU execution prevent normal startup; there is no detection-disabled operating mode.

## Build, test, and deploy

Use the repository scripts for all builds and deployments. From the repository root:

```bash
bash scripts/build.sh -R
ctest --test-dir build --output-on-failure
sudo bash scripts/deploy.sh -i
```

| Option | Effect |
| --- | --- |
| `build.sh -R` | Release build; writes `bin/autonomous-driving-car` |
| `build.sh -D` | Debug build with terminal debug output |
| `build.sh -P rk3576` | Explicit model target platform when automatic detection is unavailable |
| `build.sh -M` | Explicit model conversion rather than reuse |
| `build.sh -C` | Clean only: removes build outputs and clears project logs |
| `deploy.sh -i` | Install/update `/opt/autonomous-driving-car`, install/enable the unit, and start the service |
| `deploy.sh -u` / `-r` | Uninstall / uninstall then reinstall; these remove the installation directory |

Ordinary updates use **`deploy.sh -i`**. Deployment stops the service, reconciles models, preserves device configuration and existing logs/recordings, installs the binary and frontend, and restarts. Video and clients are interrupted during the restart. Independently changed source/deployed models cause a reported conflict rather than a silent overwrite.

```bash
systemctl status autonomous-driving-car --no-pager
journalctl -u autonomous-driving-car -f
```

The systemd unit uses `Type=notify`, a 10-second watchdog, and `Restart=on-failure`. It runs from `/opt/autonomous-driving-car` and restricts writable application paths to `config/`, `logs/`, and `recordings/`.

For foreground debugging, stop the installed service first to avoid competing for ports/camera, then run `sudo ./bin/autonomous-driving-car` from the repository root. Runtime paths are relative to the working directory.

## Models and camera

The canonical conversion/upload/runtime model is **`config/yolo26.rknn`**; default labels are `config/coco.names`. Models must match the selected NPU and supported YOLO26 output layout.

The build discovers `.pt` files under `config/`. One candidate is selected automatically; multiple candidates require an explicit choice:

```bash
YOLO_WEIGHTS=config/my_model.pt bash scripts/build.sh -R
```

Without PT input, the build reuses a valid existing RKNN model. Conversion needs the Ultralytics CLI and compatible RKNN tooling, normally in `.venv`. Successful builds clean the selected project-owned PT input and conversion cache copies. Failed builds retain inputs; externally supplied weights are preserved. PT conversion inputs are not deployed.

In **System Configuration / 系统配置**, select camera, resolution/FPS combination, encoder, and decoder. Modes come from device capabilities; 1280×720 at 30 FPS is a selectable configuration, not a fixed assumption. Drivers can negotiate a different effective rate. Capture decoding currently uses software; continuous autofocus is enabled where supported.

The frame ring moves buffer ownership between stages and reuses pools. Inference results remain associated with their captured frame. The renderer favors newer completed results instead of waiting indefinitely for out-of-order workers. H.264 live streaming and MP4 share encoded output; browser playback uses a bounded MSE buffer with catch-up logic.

## Web interface and network settings

| Page / service | Address | Purpose |
| --- | --- | --- |
| Home | `http://<board-ip>/` | Annotated video and resource monitoring |
| System Configuration | `/config` | Camera, YOLO, Web network/VOFA, CAN, CPU policies, network tests, intelligent following |
| Driving Logs | `/logs` | Logs, recordings, and detection reports |
| DBC viewer | `/dbc` | CAN decode view, linked from CAN configuration |
| HTTP | TCP **80**, fixed | Pages, APIs, video, and file operations |
| VOFA | TCP **6667** by default, configurable | JustFloat telemetry to PC clients |

Management pages, writes, and protected file operations use local administrator authentication (`root` or an account in the `sudo` group). Authentication uses HTTP Basic; provide a trusted network or a separately configured secure access layer. Some status APIs are publicly readable. Native protected video/archive downloads use scoped, short-lived session cookies.

**Web & VOFA Settings / Web与VOFA设置** configures the system interface used to reach the Web UI: Web interface, static/DHCP mode, IP, netmask, and gateway. HTTP remains on port 80 and listens on the board's configured IPv4 interfaces; selecting a management interface does not restrict HTTP to that interface. IP changes affect the actual system interface and can disconnect the browser.

The same continuous field area contains VOFA enable state, **VOFA端口**, and send rate. Its **one Save button** saves VOFA first and applies IP last. If VOFA saving fails, IP is not applied. These are sequential saves: an IP-save failure does not roll back an already saved VOFA change.

The former ordinary TCP **6666** service, client buffers, transmit queue, and configuration fields have been removed. Legacy `tcp_port`/`max_clients` file entries are ignored. Legacy `tcp_bind` is read only to migrate a management interface when `ip_ifname` is absent; subsequent saves omit these legacy keys.

The CAN master switch persists immediately. Turning it off hides its card, stops application CAN traffic, and clears pending frames while retaining interface configuration. CPU frequency settings apply per policy; actual thermal/kernel limits may be below requested values.

## Intelligent following

**Intelligent Following / 智能跟随** groups tracking, speed strategy, steering, and automatic/manual speed parameters. Groups save independently. Only the active speed strategy's parameter group is displayed; routine tuning preserves unrelated edits and loop state. Selecting another target or changing enable state performs the corresponding reset.

The controller associates detections by selected class and nearby normalized center. It is not an identity/re-identification tracker. Selecting a target captures its normalized box area as a relative-size reference.

```text
horizontal_error = 2 * (target_center_x / image_width - 0.5)
size_error       = clamp((reference_area - current_area) / reference_area, -1, 1)

automatic speed = bounded size PD output, at least zero
manual speed    = configured forward-speed setpoint
steering        = bounded horizontal-error PD output

left_target     = forward_speed + steering
right_target    = forward_speed - steering
wheel_command   = PI(target_wheel_speed - measured_wheel_speed)
```

Wheel targets and steering correction use **m/s**. Both wheel targets are scaled together when necessary to preserve their ratio under the speed limit. Each wheel PI produces a normalized command in `[-1, 1]`; parameters are shared but feedback and integral state are independent. `wheel_integral_limit` bounds each I-term contribution in `0..1`; zero disables integration. Derivative filtering and conditional-integration anti-windup are implemented.

Box area is a relative-size cue, not calibrated distance. Target pose, occlusion, and zoom can change it. Automatic and manual strategies both retain the inner wheel feedback loops.

New inference observations trigger control using capture-time differences. Unchanged frames do not advance PID again; idle wakes check stop conditions every 50 ms. Target loss, stale detections/feedback, capture reset, disabled CAN, and failed FIFO admission inhibit motion. Setpoints may still be previewed without an actuator adapter.

### Motor CAN integration

Motor encoding/decoding belongs to [the shared CAN module](source/can/can_socket.c), using the same interfaces, sockets, filters, and receive path as other CAN traffic. There is no standalone motor CAN module or socket.

Start from the **disabled** [example profile](config/car_motor_can.example.conf). Replace its illustrative IDs, layout, scaling, direction, and stop bytes with the manufacturer's documented protocol before enabling it. The runtime file is `config/car_motor_can.conf` under the installation directory; an absent, disabled, or invalid profile does not bind the adapter or send motor frames.

The profile accepts `key value` or `key=value` and `#` comments:

| Scope | Keys |
| --- | --- |
| Global | `enabled`, `interface`, `feedback_pair_timeout_ms` |
| Each `left.` / `right.` command | `cmd_id`, `cmd_extended`, `cmd_fd`, `cmd_length`, `cmd_template` |
| Command field | `cmd_offset`, `cmd_width`, `cmd_big_endian`, `cmd_scale`, optional `cmd_bias` |
| Explicit stop payload | `stop_template` |
| Feedback frame | `fb_id`, `fb_extended`, `fb_fd`, `fb_length` |
| Feedback field | `fb_offset`, `fb_width`, `fb_big_endian`, `fb_scale` |

Fields are signed 16/32-bit integers. Encoding uses `raw = round(command * cmd_scale + cmd_bias)`; decoding uses `wheel_mps = raw * fb_scale`. Include gearing, wheel diameter, units, and polarity in the feedback conversion. Matching non-overlapping left/right fields can share a frame. Invalid/duplicate/unknown keys and incompatible layouts are rejected.

Motor commands use direct nonblocking writes rather than a stale-command queue. Feedback is published as a fresh left/right pair, timestamped with the older wheel sample. Protocols needing a startup state machine, checksums, or unsupported payload types need a specific adapter. Configure the drive watchdog and verify stop behavior and direction with unloaded wheels before actual operation; a broken CAN link cannot deliver a stop frame.

### Real-time readiness

The worker requests `SCHED_FIFO` priority **20**. Motion requires successful scheduling admission, a valid adapter, enabled CAN, and fresh detections/real feedback. Read `GET /api/follow`: `status.fifo`, `status.rt_error`, and `status.rt_kernel` report actual readiness; `motor` reports configuration, interface availability, errors, and counters.

FIFO success does not imply PREEMPT_RT. The validation board has not been upgraded to a PREEMPT_RT kernel, and scheduling admission can vary with permissions and group limits. No ordinary-scheduler motion fallback is enabled. Building/deploying this project does not install a kernel or reboot the board.

## VOFA+ setup

The **board is the TCP server**; VOFA+ on the PC is the **TCP client**. Telemetry reads controller snapshots and never runs a control iteration or enables motors.

1. Open **系统配置 → Web与VOFA设置**. Select **启用 VOFA**, choose **VOFA端口** (default `6667`, allowed `1024..65535`) and **发送频率 (Hz)** (default `30`, allowed `1..100`), then use the top Save button.
2. In VOFA+, select **TCP Client**, the board IP and saved VOFA port, and the **JustFloat** data engine. For example, a board at `192.168.1.5` with port `6667` accepts a connection to `192.168.1.5:6667`.
3. Add two Waveform components. Use time `T` as X; bind CH0/CH1 to the speed plot and CH2/CH3 to the steering-correction plot. Set the sample interval to `1 / rate_hz` seconds: about `33.333 ms` at 30 Hz.
4. After changing the port, reconnect the client to the new port. Update the plot interval when changing rate. Select **停用 VOFA** and save to close telemetry connections.

| Channel | Meaning | Calculation | Unit |
| --- | --- | --- | --- |
| CH0 | Target speed / 目标速度 | `(left_target + right_target) / 2` | m/s |
| CH1 | Actual speed / 实际速度 | `(measured_left + measured_right) / 2` | m/s |
| CH2 | Target steering correction / 目标转向修正 | `(left_target - right_target) / 2` | m/s |
| CH3 | Actual steering correction / 实际转向修正 | `(measured_left - measured_right) / 2` | m/s |

Steering channels are differential wheel-speed correction, not steering angle or yaw rate. Missing/stale wheel feedback produces IEEE `NaN` for actual channels; the HTTP snapshot uses `null`. No fabricated zero-speed measurements are supplied.

Each packet contains four little-endian 32-bit floats and the tail `00 00 80 7F`: **20 bytes**, with no text header or channel names. Samples repeat the current state between inference updates. Up to four clients are supported; pending application output is bounded, and slow clients may lose samples or be disconnected. Sampling rate is subject to scheduling.

The webpage shows configuration only; view plots in VOFA+. A busy replacement port rejects the save and retains previous settings/connections. Settings persist in `config/vofa.conf` and are preserved by normal deployment.

## Recordings and detection reports

Driving Logs combines logs, MP4 recordings, and daily TXT results. It supports text preview, finalized video preview, individual download/delete, category packs/clears, and a whole-page ZIP. Active-recording download closes its current segment and continues in a new segment; bulk deletion retains the active recording, including its finalization window. Inactive interrupted or malformed MP4 files are deleted too; they remain excluded from preview and whole-page archives. Historical migration tools do not imply automatic deletion of all old artifacts.

| Output | Location / naming |
| --- | --- |
| Information/error logs | `logs/…/log_info_YYYYMMDD_HHMMSS_mmm.log`, `log_error_YYYYMMDD_HHMMSS_mmm.log` |
| Annotated video | `recordings/…/video_YYYYMMDD_HHMMSS_mmm.mp4` |
| Completed inference results | `recordings/detections/YYYYMMDD/result_YYYYMMDD.txt` |

Every completed inference, including a zero-target result, is submitted to the report writer. This does not mean every camera frame completes inference or appears in the stream/recording. New output does not create recognition photos or per-frame JSON.

Example TXT layout:

```text
2026-10-28T20:19:17.653+0800：num[1] size[1280 720] FocalLength[raw:160]
    person 0.891602 position[572.000 1279.000 3.000 690.000]
```

Each frame header contains local capture time with timezone, detection count, image width/height, and lens position (`raw:<value>` or `unknown`). Each object contains name, confidence, and **`X1 X2 Y1 Y2`** in original-image coordinates after letterbox restoration and clipping. The TXT currently has no separate camera identity field and no millimeter focal-length calibration.

## Persistent configuration

Paths are relative to the runtime working directory (`/opt/autonomous-driving-car` for the service).

| File | Purpose |
| --- | --- |
| `config/config.txt` | Camera, YOLO, CAN, and Web interface IP configuration |
| `config/follow.conf` | Following parameters; moving state is not restored at startup |
| `config/car_motor_can.conf` | Actual motor protocol profile; not supplied/enabled by default |
| `config/vofa.conf` | VOFA enable state, port, and sample rate |
| `config/cpu_freq.conf` | CPU governor/min/max values per policy |
| `config/yolo26.rknn` | Canonical model |
| `config/coco.names` | Default class labels |
| `config/*.dbc` | Per-interface DBC databases |

Use the deployed configuration for runtime changes. Keep models, device settings, logs, and recordings when moving an existing installation. Do not replace them with a clean checkout merely to update source code.

## API and diagnostics

These are selected endpoints; [http.c](source/http/http.c) and individual HTTP handlers define routing and authorization. Paths are relative to the board's HTTP origin.

| Endpoint | Purpose |
| --- | --- |
| `GET/POST /api/config` | Configuration snapshot / apply selected module; exposes fixed `http_port: 80` |
| `GET/POST /api/follow` | Following parameters, detections, readiness, motor diagnostics |
| `GET/POST /api/vofa` | Telemetry configuration, channels, listener/client counters |
| `GET /api/system`, `GET/POST /api/cpu_freq` | Resource monitoring and CPU policies |
| `GET /api/network/ifaces`, `GET /api/network` | Interface discovery and network state |
| `GET /api/can/ifaces`, `/api/can/frames`, `/api/can/decoded` | CAN interfaces, raw frames, decoded signals |
| `POST /api/can/send`, `/api/can/toggle`, `/api/can/dbc` | Manual transmission, interface toggle, DBC upload |
| `GET /api/camera`, `/api/video/devices`, `/api/video/caps` | Camera configuration/capabilities |
| `GET /api/video/stats` | Capture/drop/display/encoding counters and processing age |
| `GET /api/rec/status`, `/api/rec/list` | Recording state and file list |
| `POST /api/rec/start`, `/api/rec/stop` | Recording control |
| `GET /api/detections/reports`, `/api/logs` | Daily report and log lists |
| `GET /api/files/preview`, `/api/files/pack`, `POST /api/files/clear` | Text preview, whole-page archive, managed-file cleanup |

Protected native downloads/previews require the browser's authenticated session workflow. Browser video measurements are available through `window.getCarVideoDiagnostics()`; pipeline age is not a direct camera-to-screen latency measurement.

```bash
systemctl status autonomous-driving-car --no-pager
journalctl -u autonomous-driving-car -n 100 --no-pager
curl -s http://127.0.0.1/api/config
curl -s http://127.0.0.1/api/follow
curl -s http://127.0.0.1/api/vofa
curl -s http://127.0.0.1/api/video/stats
```

| Symptom | Check |
| --- | --- |
| Startup fails at YOLO | NPU driver/runtime compatibility, RKNN platform/layout, canonical model, labels |
| No video / increasing delay | Negotiated mode, camera errors, pipeline counters, thermal limits, network and MSE buffer |
| No motor output | Enabled state, selected target, profile validation, CAN, FIFO error, feedback freshness |
| Motor socket online but no wheel speed | Feedback IDs/format/offsets/endian order/scaling and actual incoming frames |
| VOFA cannot connect | Saved port, enabled/listening state, board IP, firewall, client mode |
| VOFA actual curves absent | `feedback_valid`, real wheel feedback; unavailable values are `NaN` |
| CPU clock below requested value | Selected policy, effective limits, temperature and kernel thermal controls |
| Model synchronization conflict | Resolve independent project/deployment changes before retrying |

CTest covers frame ownership/queues, H.264 timeline, color conversion, recording splits, reports, model synchronization, files, CPU settings, CAN lifecycle, Web configuration, and VOFA TCP lifecycle. Passing these checks does not establish real-motor protocol compatibility, long-term video stability, PREEMPT_RT readiness, or PC VOFA+ UI acceptance.

## License

[GPL-3.0-or-later](LICENSE).
