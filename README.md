# Autonomous Driving Car

Autonomous Driving Car is a Linux vehicle platform designed around a unified perception, reasoning, and control architecture. Its hardware/system target is a shared computing platform running a Linux real-time kernel. The software design combines conventional motion-control algorithms, YOLO object detection, and a large language model (LLM) reasoning layer.

The implemented driving path currently uses YOLO26 target position and relative size to follow a selected target. Conventional PD and PI controllers produce bounded left/right wheel commands and close the speed loops using measured CAN feedback. The LLM layer is planned and reserved: no LLM provider, model invocation, or reasoning-to-control connection is implemented. The browser also provides live video, recording, per-frame detection reports, DBC decoding, and device configuration.

The application uses C11, with C++/OpenCV for overlays and Rockchip RKNN for inference. Hardware capabilities are discovered from the running system. A compatible model, NPU runtime, camera, and motor protocol profile are required for their respective functions. Motor control starts stopped and remains inhibited until its real-time, CAN, detection, and feedback checks pass.

## System architecture

The system follows a unified perception/reasoning/control design on a Linux real-time kernel target. Perception and driving control share the computing platform, while scheduling and hardware capabilities are detected at runtime.

| Layer | Responsibility | Current implementation |
| --- | --- | --- |
| Linux real-time platform | Schedule the perception, communication, and periodic motion-control work. | The controller requests FIFO scheduling. The validation board still runs a kernel without PREEMPT_RT; a real-time kernel has not been installed. |
| Conventional motion control | Combine target following, steering, wheel-speed feedback, and bounded motor commands. | Steering PD, automatic speed PD, shared wheel-speed PI parameters, and SocketCAN command/feedback transport are implemented. Real motor output remains disabled. |
| YOLO perception | Detect the selected target and restore its coordinates to the original image. | YOLO26 runs through Rockchip RKNN and supplies frame-matched observations to the controller. |
| LLM reasoning | Provide the planned higher-level language/reasoning component of the autonomous-driving design. | Reserved architecture only; no LLM runtime or connection is integrated, and no LLM drives the motor outputs. |

## Capabilities

- Select a detected target and configure automatic or manual forward speed, steering PD, and shared wheel-speed PI from the browser.
- Connect compatible motor drives through a configurable SocketCAN adapter with explicit command, feedback, and stop-frame formats.
- Send target/measured speed and steering-correction channels to VOFA+ over a separate TCP telemetry connection.
- Capture V4L2 video, run YOLO26 inference, and display the corresponding annotated frames through H.264/MSE or MJPEG.
- Record annotated MP4 video and save every completed inference to a daily TXT report, including frames with no detections.
- Manage logs, recordings, and results together: preview, individual download/delete, per-section archives, and whole-page ZIP download/delete.
- Discover CAN/CAN FD interfaces, configure filters and bitrates, upload DBC files, and inspect raw or decoded traffic.
- Monitor CPU, memory, disk, NPU, GPU, and network state; configure CPU frequency policies and TCP/IP settings.
- Use an English or Chinese interface. Runtime changes preserve device-specific configuration, and systemd monitors critical worker threads.

## Requirements

A Linux board needs working V4L2, SocketCAN, and Rockchip NPU drivers for the enabled hardware. The current validation board is a Radxa Rock 4D with RK3576. Platform detection and NPU core selection also contain mappings for other Rockchip devices, but a mapping alone does not establish driver, model, or encoder compatibility.

Build dependencies are GCC/G++ with C11/C++11 support, CMake 3.10 or newer, Python 3, libsystemd, libnl-3/libnl-route-3, libjpeg, OpenCV core/imgproc, and the board-compatible `librknnrt.so`. Software H.264 encoding requires an FFmpeg build containing `libx264`; hardware encoding requires an available RKMPP or V4L2 encoder.

On Debian/Ubuntu, install the development packages:

```bash
sudo apt install build-essential cmake python3 pkg-config \
  libsystemd-dev libnl-3-dev libnl-route-3-dev \
  libjpeg62-turbo-dev libopencv-dev libavcodec-dev libavutil-dev
```

Install the NPU runtime from the SDK appropriate to the board and kernel. YOLO inference is required by the application: a missing/incompatible RKNN model or unavailable NPU prevents a normal startup.

## Build and deploy

Run commands from the repository root:

```bash
bash scripts/build.sh -R
ctest --test-dir build --output-on-failure
sudo bash scripts/deploy.sh -i
```

The build writes `bin/autonomous-driving-car`. Useful build options are `-D` for a debug build, `-P rk3576` for an explicit platform, and `-M` for explicit model conversion. `-C` clears build output and logs; it is not needed for an ordinary rebuild.

Deployment installs into `/opt/autonomous-driving-car`, updates the systemd unit, and starts the service. It preserves device configuration and uploaded models, detects conflicting model changes, and retains the configured CPU-frequency and follow-control settings. It restarts the service, so expect a brief interruption of live video.

For an existing installation, retain its `config/`, `logs/`, and `recordings/` when migrating to the new installation directory. Stop the previous service before starting the renamed service.

```bash
systemctl status autonomous-driving-car --no-pager
journalctl -u autonomous-driving-car -f
```

For a foreground run, use `sudo ./bin/autonomous-driving-car` from the repository root. Relative paths such as `config/` and `html/` are resolved from the working directory. Use `deploy.sh -i` to update an installation; uninstall/reinstall actions remove the deployment directory and are not routine update commands.

## Model and camera setup

Conversion, normal runtime loading, and model uploads share **`config/yolo26.rknn`**. Class labels use `config/coco.names` unless configured otherwise. The model must match the target NPU and supported YOLO26 output layout.

The build discovers `.pt` weights under `config/`. A single candidate is converted automatically; multiple candidates stop the build until one is selected with `YOLO_WEIGHTS`. With no PT input, a valid existing RKNN model is reused. Conversion requires the Ultralytics CLI and compatible RKNN conversion tools, normally installed in `.venv`.

```bash
YOLO_WEIGHTS=config/my_model.pt bash scripts/build.sh -R
```

After a successful build, the selected project-owned PT input and conversion cache copies are removed. Failed builds retain their inputs, and externally supplied weight files are preserved. Conversion weights are not deployed. Source/deployed model conflicts are reported rather than silently overwritten.

Choose the camera, combined resolution/frame-rate mode, encoder, and decoder in System Configuration. Resolution and FPS come from device capabilities and saved settings: **1280 x 720 at 30 FPS is configurable, not hardcoded**. The active mode may differ if the driver negotiates a lower frame rate. Available capture decoding is software; hardware encoding is offered only when a usable backend exists. Continuous autofocus is used when the camera supports it.

## Browser interface

Open `http://<board-ip>/` for Home, `/config` for System Configuration, and `/logs` for Driving Logs. Management pages, writes, and protected downloads require a local administrator account: `root` or an account in the `sudo` group. The browser uses HTTP Basic authentication; use the application on a trusted network or behind a separately configured secure access layer.

In System Configuration, **Intelligent Following** groups and saves the vehicle controls independently:

| Group | Settings |
| --- | --- |
| Tracking settings | Stopped/enabled state and target selection from current detections. |
| Speed Strategy | Automatic/manual mode and maximum wheel speed. |
| Steering parameters | Maximum wheel-speed correction, center deadband, proportional and derivative coefficients. |
| Automatic speed control | Proportional and derivative coefficients for relative target size. |
| Manual speed control | Target speed, shared wheel proportional/integral coefficients, and integral limit. |

On desktop, Steering parameters occupy the left box of the second row. The active automatic/manual parameter group occupies the right box. Only the group belonging to the selected speed mode is shown; the manual speed input is hidden in automatic mode. Narrow displays stack the groups in reading order.

Saving one group does not submit another group's unsaved edits. Ordinary tuning preserves the target, capture timing, and unrelated loop state. Selecting a new target or changing the enabled state performs the corresponding controller reset. Following always starts **stopped after a service restart**.

The top-level CAN switch persists immediately. Disabling it hides the CAN configuration card, stops application CAN acquisition/transmission, and clears pending frames while retaining interface settings. The DBC page is linked from the CAN card.

## Following and wheel control

The independent controller is implemented in `source/control/car_sport_ctrl.c` and `include/control/car_sport_ctrl.h`.

For target center `cx`, image width `W`, normalized target-box area `A`, and captured reference area `Aref`:

```text
horizontal_error = 2 * (cx / W - 0.5)
size_error       = clamp((Aref - A) / Aref, -1, 1)

automatic mode: forward_speed = bounded size PD output, at least zero
manual mode:    forward_speed = configured speed setpoint

correction = bounded steering PD output
left_target  = forward_speed + correction
right_target = forward_speed - correction
```

Selecting a detection records its normalized reference area. If no reference is saved, the first matching target establishes one. A smaller target increases automatic forward speed; a larger target reduces it toward zero. Manual speed bypasses size-based speed control. Bounding-box area is a relative-size cue, not a calibrated distance measurement; target pose, occlusion, and camera zoom can affect it.

Steering correction and wheel targets are in **m/s**; no wheel-track input is required. Limiting scales both wheel targets together to retain their differential ratio. Original-image coordinates are used throughout. Target association follows the nearest center within the selected class and a normalized distance gate; it is not an identity/re-identification tracker.

Each wheel PI compares its target with **measured signed wheel speed**, then produces a normalized actuator command in `[-1, 1]`. Both wheels share coefficients but retain independent feedback, integral state, and output. The inner wheel feedback loops operate in both speed modes.

`wheel_integral_limit` bounds the magnitude of each wheel's I-term contribution. Its range is `0..1`: `1` is the full normalized command range, and `0` clears/disables integration. A reduced limit applies even during output saturation. Filtered derivatives, conditional-integration anti-windup, and total output limits remain active.

New inference results trigger controller updates using capture-time differences. The controller consumes current observations without building a stale command backlog. Idle wakes check stopping conditions every 50 ms without advancing PID on an unchanged frame. Target loss, stale detections/feedback, a capture-session reset, disabled CAN, or unavailable real-time scheduling inhibit motion output.

## CAN motor integration

Motor protocol mapping belongs to the shared CAN module under `source/can/` and `include/can/`. Motor commands and measured feedback use the application's common CAN transmit/receive path, alongside its ordinary CAN traffic and DBC decoding. There is no separate motor CAN socket or receiver module.

No real motor profile is supplied. A missing, disabled, or invalid `config/car_motor_can.conf` does not bind a motor adapter or transmit motor frames. Start with the **disabled** [example profile](config/car_motor_can.example.conf), then replace its illustrative values with the drive manufacturer's documented protocol.

```bash
# In an installed system, edit the deployed runtime profile.
sudo cp config/car_motor_can.example.conf \
  /opt/autonomous-driving-car/config/car_motor_can.conf
sudoedit /opt/autonomous-driving-car/config/car_motor_can.conf
sudo systemctl restart autonomous-driving-car
```

The profile accepts `key value` or `key=value` lines and `#` comments. General keys are `enabled`, `interface`, and `feedback_pair_timeout_ms`. Each `left.`/`right.` wheel uses these mappings:

| Mapping | Keys and meaning |
| --- | --- |
| Command frame | `cmd_id`, `cmd_extended`, `cmd_fd`, `cmd_length`, `cmd_template`. |
| Command field | `cmd_offset`, `cmd_width` (`16`/`32`), `cmd_big_endian`, `cmd_scale`, optional `cmd_bias`. |
| Stop frame | `stop_template`: required explicit bytes of exactly the command-frame length. |
| Feedback frame | `fb_id`, `fb_extended`, `fb_fd`, `fb_length`. |
| Feedback field | `fb_offset`, `fb_width` (`16`/`32`), `fb_big_endian`, `fb_scale`. |

IDs accept `0x` notation. Classic CAN and legal CAN FD lengths are supported. Data fields are signed integers. Command encoding applies `raw = round(normalized_command * cmd_scale + cmd_bias)`; feedback decoding applies `wheel_mps = raw * fb_scale`. Include wheel diameter, gearing, units, and polarity in the real feedback conversion. Commanded speed must never be substituted for measured feedback.

Left/right fields can share one command frame when their ID, format, templates, and length match and their fields do not overlap. Shared feedback frames require consistent formats and non-overlapping fields. Unknown/duplicate keys, invalid field bounds, incompatible templates, or missing stop bytes reject the profile. The adapter sends no guessed initialization or enable sequence.

Direct nonblocking transmission avoids an application command queue. Feedback is published only as a fresh left/right pair, using the **older** wheel timestamp so one wheel cannot keep stale feedback for the other wheel alive. The profile's pairing timeout and the controller's stale-data timeout serve different checks.

Configure CAN bitrate/FD mode, documented stop behavior, and the motor drive's independent command watchdog before operation. Verify direction and unit conversion with wheels safely unloaded, then select a target and explicitly enable following. Motor drives needing a state machine, startup handshake, checksums, or another unsupported payload format require a protocol-specific adapter; this profile is not a universal CAN motor driver.

### Real-time readiness

The controller requests `SCHED_FIFO` priority 20. **Motion remains inhibited unless the real-time request succeeds**, global CAN is enabled, and detections and measured wheel feedback are fresh. Profile loading or connecting the CAN cable does not bypass these checks. A configured adapter may send its documented stop frames while motion is inhibited; missing/disabled profiles send no motor frames.

The validation board currently runs a kernel with `CONFIG_PREEMPT_RT` disabled. `GET /api/follow` reports actual startup scheduling admission through `fifo` and `rt_error`, and kernel detection through `rt_kernel`. FIFO admission can change with service permissions and RT-group limits; it does not establish that PREEMPT_RT is enabled. Motor output remains disabled, and no ordinary-scheduler fallback is enabled.

Investigate scheduling permissions and RT-group/cgroup restrictions separately from kernel replacement. Enabling PREEMPT_RT requires a compatible kernel and a planned, validated reboot; this deployment does not change boot parameters or reboot the board. `GET /api/follow` exposes scheduling readiness and a top-level `motor` object containing `configured`, `online`, `interface`, `error`, and transmit/receive counters. A physical emergency stop and motor-drive watchdog remain necessary because a failed CAN link cannot deliver a stop frame.

## VOFA+ telemetry

System Configuration includes a VOFA section inside Network Settings for enabling telemetry, selecting its TCP port, and setting a sampling limit of `1..100 Hz`. It is disabled by default and uses port **6667**, independently of the application's CAN/TCP data port. The board is a TCP server; VOFA+ on the computer is the TCP client. Saving the card persists these settings in `config/vofa.conf`. Telemetry configuration does not enable following or motor output.

The stream uses the official [JustFloat format](https://www.vofa.plus/docs/learning/dataengines/justfloat/): each 20-byte frame contains four little-endian 32-bit floats followed by `00 00 80 7F`. There are no text headers or channel-name packets. Channel positions are fixed:

| Channel | Curve | Source | Unit |
| --- | --- | --- | --- |
| 0 | Speed target | `(left_target + right_target) / 2` | m/s |
| 1 | Speed actual | `(measured_left + measured_right) / 2` | m/s |
| 2 | Steering target | `(left_target - right_target) / 2` | m/s |
| 3 | Steering actual | `(measured_left - measured_right) / 2` | m/s |

Steering here is the differential wheel-speed correction already used by the controller. It is not a steering angle or yaw rate. Actual channels require fresh, signed wheel-speed feedback from the motor adapter. Missing or stale feedback produces IEEE `NaN` in the TCP stream and `null` in the HTTP snapshot; it is never replaced by a fabricated zero-speed measurement.

To create the two comparison plots in VOFA+:

1. Enable VOFA in Network Settings, choose a free port, and save it. On the computer, select **TCP Client**, connect to the board's IP and the saved port, and select the **JustFloat** data engine.
2. Add two Waveform widgets. In each widget's right-click axis settings, use time `T` for X. Bind channels **0 and 1** to the first widget's Y axis for target/actual speed, and channels **2 and 3** to the second for target/actual steering correction.
3. Set the VOFA sampling interval to `1 / rate_hz` seconds (about `33.333 ms` at 30 Hz). Use Auto scale and keep the displayed sample window at its latest position. The official [waveform guide](https://www.vofa.plus/docs/learning/widgets/wave/) describes axis bindings and the sampling-interval control.

The existing TCP module manages both the ordinary data endpoint and the VOFA endpoint in its shared event loop. Their configurable ports keep CAN data and JustFloat frames separate. Telemetry periodically samples the current control state, holding target values between control updates; it does not trigger PID updates or change the inference-driven control period. Sampling follows the configured rate subject to Linux scheduling. Up to four VOFA clients can connect. A slow client may lose telemetry samples or be disconnected so that it cannot build an unbounded application backlog or delay control.

## Video, files, and detection records

The H.264 stream and MP4 recorder share encoded output. The browser presents frames with their matching YOLO overlays. Inference concurrency adapts automatically; camera resolution/FPS, confidence/NMS, and model uploads remain configurable.

Chrome uses a bounded adaptive MSE playback buffer and controlled catch-up. Reducing that buffer does not eliminate camera, decoding, inference, encoding, or network delay. **100 ms camera-to-screen latency is not a demonstrated guarantee.** Home shows playback measurements; additional diagnostics are available with `window.getCarVideoDiagnostics()` and `GET /api/video/stats`.

The Driving Logs page combines Logs, Recordings, and Results. Text previews read a bounded prefix; finalized MP4 previews use native byte-range playback. Downloading the active recording first closes its current segment and continues recording into a new file. Whole-page ZIP archives store already-compressed video without recompression. Bulk deletion requires confirmation and retains the active recording.

| Output | Filename |
| --- | --- |
| Logs | `log_info_YYYYMMDD_HHMMSS_mmm.log`, `log_error_YYYYMMDD_HHMMSS_mmm.log` |
| Recordings | `video_YYYYMMDD_HHMMSS_mmm.mp4` |
| Detection results | `result_YYYYMMDD.txt` |

Files are stored in date directories; collision suffixes can be added. Every completed inference, including a zero-target frame, contributes to the daily UTF-8 TXT report. Each frame has capture time, target count, original image dimensions, and available focal/lens metadata; each target has its label, confidence, and coordinates in **`X1 X2 Y1 Y2`** order. Boxes are mapped back from model letterboxing and clipped to original-image bounds. No recognition photos or per-frame JSON files are generated.

Lens position is reported as calibrated millimeters only when calibration is available; otherwise it is a device raw value or unknown. Autofocus position alone is not a millimeter focal length. Clearing today's report removes saved history; later inference frames may create a new report.

## Configuration, APIs, and source layout

| Path | Purpose |
| --- | --- |
| `config/config.txt` | Device-specific camera, CAN, TCP/IP, and inference settings. |
| `config/follow.conf` | Following parameters; enabled/moving state is not restored on restart. |
| `config/car_motor_can.conf` | Actual motor protocol profile; absent/disabled by default. |
| `config/vofa.conf` | Persisted VOFA enable state, TCP port, and sampling rate. |
| `config/cpu_freq.conf` | Per-policy CPU governor and requested limits. |
| `config/yolo26.rknn`, `config/coco.names` | Canonical RKNN model and labels. |
| `source/control/`, `include/control/` | Motion-control algorithms and interfaces. |
| `source/telemetry/`, `include/telemetry/` | Nonblocking VOFA TCP telemetry service. |
| `source/video/`, `source/yolo/` | Capture, frame ring, inference, overlays, encoding, and results. |
| `source/can/`, `source/tcp/`, `source/http/` | Shared CAN/motor transport, TCP data, and browser/API service. |
| `html/`, `scripts/` | Web interface and build/deployment utilities. |

Heavy capture/inference/encoding work uses the detected higher-capacity CPU group; control/network/background work uses the lower-capacity group. Unknown or homogeneous CPU layouts retain the allowed CPU set. CPU-frequency policies are handled independently, and effective thermal limits can be lower than requested limits.

Key HTTP endpoints:

| Endpoint | Use |
| --- | --- |
| `GET /api/follow` | Control configuration, current targets, readiness, and motor-adapter diagnostics. |
| `POST /api/follow` | Save a validated subset of control parameters. |
| `GET/POST /api/vofa` | Read telemetry status/channels or save its enable state, port, and rate. |
| `GET/POST /api/config` | Read/apply the selected configuration module. |
| `GET /api/system`, `GET/POST /api/cpu_freq` | Monitoring and CPU policy controls. |
| `GET /api/can/ifaces`, `/api/can/frames`, `/api/can/decoded` | Discover CAN and inspect received data. |
| `POST /api/can/send`, `/api/can/toggle`, `/api/can/dbc` | Manual CAN transmission, interface state, and DBC upload. |
| `GET /api/video/devices`, `/api/video/caps`, `/api/video/stats` | Camera discovery, capabilities, and pipeline measurements. |
| `GET /api/rec/status`, `/api/rec/list` | Recording state and saved files. |
| `POST /api/rec/split` | Close the current segment and continue recording. |
| `GET /api/detections/reports`, `GET /api/logs` | Daily TXT and log lists. |
| `GET /api/files/preview`, `GET /api/files/pack`, `POST /api/files/clear` | Preview text, download a whole-page archive, or clear managed files. |

Management writes and protected file operations require administrator authentication. Native video/archive downloads use scoped, short-lived cookies created by authenticated session endpoints. API paths are relative to the board's HTTP origin; use the browser workflow for preview/download session creation.

## Troubleshooting and validation

| Symptom | Check |
| --- | --- |
| Service fails during YOLO startup | Correct `librknnrt.so`, NPU driver, target platform, model path, model format, and labels. |
| No motor output | Stopped state, profile validation, SocketCAN interface, FIFO error, feedback age, target selection, and CAN enable state in `/api/follow`. |
| Adapter is online but wheels do not report speed | Online means the socket is open; verify feedback IDs, frame formats, offsets, endian order, scaling, and real traffic. |
| VOFA has no actual-speed or actual-steering curve | Check `feedback_valid` and real motor feedback; unavailable measurements are transmitted as `NaN`. |
| VOFA cannot connect or plots incorrect values | Confirm telemetry is listening, use its saved port and the JustFloat engine, and bind channels 0/1 and 2/3 to separate plots. |
| Video stalls or delay grows | Negotiated camera mode, processing age, browser buffer/drop measurements, thermal throttling, and network conditions. |
| CPU frequency differs from the request | Selected policy, effective min/max limits, temperature, and kernel thermal controls. |
| Model synchronization stops | Resolve independent source/device changes; do not overwrite one side blindly. |

Build and run the existing CTest suite before deploying changes. Hardware-free transport/control checks validate packet mapping and loop behavior, but do not certify a particular motor protocol or a vehicle's braking behavior. Keep motor integration disabled until its documented profile, feedback conversion, stopping behavior, and real-time prerequisites have been verified.

## License

GPL-3.0-or-later.
