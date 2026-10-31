# Autonomous Driving Car

A Linux vehicle application for camera capture, RKNN YOLO26 detection, visual target tracking, differential wheel following, and CAN communication. A browser interface provides live video, device configuration, monitoring, and file management.

The hardware validation platform is **Radxa ROCK 4D / RK3576**. The backend uses C11/C++11, the frontend uses HTML/CSS/JavaScript, and the build and deployment tools use Bash.

## Features

- V4L2 camera selection, resolution/frame-rate configuration, and autofocus where supported.
- NPU inference with adaptive worker allocation and annotations matched to each rendered frame.
- Click-to-select visual tracking with numbered objects and appearance-assisted recovery.
- Intelligent following with automatic/manual speed, steering PD, and independent wheel PI control.
- Shared SocketCAN/CAN FD transport, DBC decoding, interface configuration, and configurable motor integration.
- H.264/MSE and MJPEG live video, annotated MP4 recordings, and daily detection reports.
- Web configuration, resource monitoring, CPU policies, network diagnostics, and Anonymous Assistant telemetry.

## Requirements

Use Linux with V4L2, a working Rockchip NPU driver, and a board-compatible `librknnrt.so`. CAN communication additionally requires a SocketCAN interface. Actual vehicle motion requires a configured motor protocol and measured wheel-speed feedback.

Build dependencies:

- C11/C++11 compiler, GNU Make, CMake **3.18+**, Bash, and the standard Linux command-line tools.
- libsystemd, libnl-3/libnl-route-3, libjpeg, and OpenCV core/imgproc development libraries.
- `flock`, `sha256sum`, `strings`, `awk`, `tar`, and `zip` for model/configuration handling and downloads.
- Optional FFmpeg libavcodec/libavutil development libraries and pkg-config for the FFmpeg encoding backend. Software H.264 uses `libx264`; hardware encoding requires a supported RKMPP or V4L2 backend.

Prepare a compatible **YOLO26 RKNN model** at `config/yolo26.rknn` before building. Use `config/coco.names` for matching class labels. The model is device-specific and is not stored in Git; working model initialization is required for normal startup.

## Build and deploy

From the repository root, use the existing scripts:

```bash
bash scripts/build.sh -R
sudo bash scripts/deploy.sh -i
```

The executable is written to `bin/autonomous-driving-car`. Deployment installs the executable, frontend, configuration, and model under `/opt/autonomous-driving-car`, installs the systemd unit, and starts the service.

| Command | Purpose |
| --- | --- |
| `bash scripts/build.sh -R` | Release build, with optimization and debug information |
| `bash scripts/build.sh -D` | Debug build with terminal debug output |
| `bash scripts/build.sh -R -P rk3576` | Explicitly select the model platform |
| `bash scripts/build.sh -C` | Remove build outputs **and clear project logs**; does not build |
| `sudo bash scripts/deploy.sh -i` | Install or update the deployment and restart the service |
| `sudo bash scripts/deploy.sh -u` | Uninstall and remove the entire installation directory |
| `sudo bash scripts/deploy.sh -r` | Uninstall, then install again |

Use `deploy.sh -i` for ordinary updates: it preserves recordings, logs, and existing device settings. The `-u` and `-r` actions remove installed configuration and recorded data along with the application.

```bash
systemctl status autonomous-driving-car --no-pager
journalctl -u autonomous-driving-car -f
```

The service uses `/opt/autonomous-driving-car` as its working directory, supervises worker heartbeats, and restarts on failure.

## Web interface

Open **`http://<board-ip>/`**. HTTP uses fixed port **80**.

| Page | Address | Purpose |
| --- | --- | --- |
| Home | `/` | Live video, visual target selection, and resource monitoring |
| System Configuration | `/config` | Camera, YOLO, CAN, Web/telemetry, CPU, and following settings |
| Driving Logs | `/logs` | Logs, recordings, detection reports, downloads, and cleanup |
| DBC viewer | `/dbc` | CAN definitions and decoded signals |

The home and DBC pages use a local Linux account through HTTP Basic authentication. Configuration, logs, and administrative actions require `root` or a member of the `sudo` group. Some read-only status APIs are public.

**Web & ANO Settings** combines Web interface IP settings and telemetry settings with one Save button. The IP settings manage the selected system network interface; the HTTP listener remains available on the board's IPv4 interfaces. Reconnect using the new address after changing the board's IP.

Web access is limited to **two source IPv4 addresses**. Multiple tabs, API requests, downloads, and video connections from one address share a slot. The slot expires 30 seconds after its last connection closes; an additional address receives HTTP 503.

## Models and deployment configuration

Upload a prepared `.rknn` model and its labels from the YOLO section of System Configuration. The model must match the board's NPU platform and the supported YOLO26 output layout. Build and deployment scripts use `config/yolo26.rknn` as the canonical model path.

| Script | Responsibility |
| --- | --- |
| [build.sh](scripts/build.sh) | Check dependencies, synchronize/validate the model, and build the executable |
| [deploy.sh](scripts/deploy.sh) | Install the application and configuration, then restart systemd |
| [model_sync.sh](scripts/model_sync.sh) | Identify the platform and synchronize prepared models and device configuration |

`model_sync.sh` is called automatically by the build and deployment scripts. It checks the RKNN header and declared target platform, compares model hashes, and copies a newly uploaded deployed model back to the source tree. If source and deployed models were independently changed, it stops with a conflict and preserves both files.

Models must be prepared externally; these scripts do not train, convert, or download them. Synchronization metadata lives directly under `config/`, without creating the retired `config/models/` directory.

During an update, existing deployed `config.txt`, `follow.conf`, `car_motor_can.conf`, `cpu_freq.conf`, and `ano.conf` settings take precedence. Labels and DBC files may be replaced by the source versions.

## Visual tracking and intelligent following

### Visual target tracking

Click a detected object on the home page to record its appearance and select it. The selected box becomes cyan; **Stop tracking** clears the selection. Object numbers, class names, confidence values, and selection markers are drawn into the video frame and included in live video and recordings.

Tracking combines appearance, box overlap, and motion. It can attempt to recover the selected object within five seconds after loss. Similar objects, lighting changes, and large viewpoint changes can cause rejection or incorrect association. Object IDs are local to the running process, and the appearance reference is cleared when tracking stops or the camera session/program restarts.

Visual selection operates independently of wheel following and does not enable vehicle motion.

### Wheel following

Configure **Intelligent Following** in System Configuration. **Automatically set speed** adjusts speed from the target's relative image size; **Manual speed** uses the configured speed. Horizontal target error drives steering PD, and each wheel uses its own PI controller with measured feedback. Wheel speeds and steering correction are expressed in **m/s**.

Motor output requires all of the following:

- Following explicitly enabled for the current session.
- Fresh target detections and measured left/right wheel-speed feedback.
- An available, enabled CAN interface and a valid, enabled motor profile.
- The controller thread successfully running with `SCHED_FIFO` priority **20**.

Following starts stopped after every service restart. Missing targets, stale feedback, or unavailable prerequisites inhibit motion. `/api/follow` reports the controller state and readiness.

Motor traffic uses the shared CAN module. Adapt [car_motor_can.example.conf](config/car_motor_can.example.conf) to the drive manufacturer's command, stop, and feedback formats before creating `config/car_motor_can.conf`. The example is disabled and is not a ready-to-use motor protocol. Special enable sequences or checksums require an appropriate adapter.

Target size is a relative visual cue, not a calibrated distance measurement. Route planning and autonomous navigation are outside the implemented feature set.

## Video, recordings, and reports

Select the camera, supported capture mode, and encoder in System Configuration. Actual resolution/frame rate and available encoding backends depend on the device and drivers. Capture decoding currently uses software.

H.264 live video and MP4 recording share an encoding session. Recording starts automatically when a session can start; manually stopping recording also ends that H.264 session and disables automatic continuation. Pipeline stages prioritize recent frames, so video and reports need not contain every captured frame.

The Driving Logs page supports previews, individual downloads, and a whole-page ZIP. Bulk deletion protects recordings that are active or being finalized. ZIP downloads exclude active or incomplete videos.

Daily TXT reports contain completed inference results, including results with no detected objects. Entries include capture time, image dimensions, lens position when available, class/confidence, and `X1 X2 Y1 Y2` coordinates in the original image.

## Anonymous Assistant

Supports **Anonymous Assistant 2.0.1**, using **ANO V8 over TCP** with the board as server and the PC as client. Configure its port and send rate under **Web & ANO Settings**. Defaults are port **8080**, **30 Hz**, and initially disabled; saved settings are restored on startup. Up to **two TCP clients** can connect.

Telemetry provides target/actual speed and steering correction. Simulated waveforms are available for checking the link; telemetry does not issue motor commands.

Official website: [ANO Tech](https://www.anotc.com/). Downloads and documentation: [Anonymous Assistant](https://gitee.com/anotc/AnoAssistant).

## Configuration and data paths

Paths below are relative to the runtime working directory, which is `/opt/autonomous-driving-car` for the installed service.

| Path | Contents |
| --- | --- |
| `config/config.txt` | Camera, YOLO, CAN, and Web IP settings |
| `config/follow.conf` | Following parameters |
| `config/car_motor_can.conf` | Device-specific motor protocol |
| `config/cpu_freq.conf` | CPU policy settings |
| `config/ano.conf` | Anonymous Assistant settings |
| `config/yolo26.rknn` | Prepared runtime model |
| `config/coco.names`, `config/*.dbc` | Class labels and CAN databases |
| `logs/YYYYMMDD/` | Application information and error logs |
| `recordings/YYYYMMDD/` | Annotated MP4 recordings |
| `recordings/detections/YYYYMMDD/result_YYYYMMDD.txt` | Daily inference reports |

**Export config** downloads a `.tar.gz` containing actual configuration files and the model. Empty directories and internal synchronization metadata are omitted. The total uncompressed file size is limited to **64 MiB**.

## Source layout

| Module | Responsibility |
| --- | --- |
| `source/core/`, `include/core/` | Startup, configuration, logging, CPU affinity, shared context |
| `source/video/`, `include/video/` | Capture, frame ownership, encoding, streaming, recording |
| `source/yolo/`, `include/yolo/` | RKNN inference, frame annotation, result reports |
| `source/control/`, `include/control/` | Visual tracking, appearance features, following, wheel control |
| `source/can/`, `include/can/` | CAN sockets, queues, DBC decoding, motor integration |
| `source/tcp/`, `include/tcp/` | Telemetry transport and interface IP helpers |
| `source/telemetry/`, `include/telemetry/` | ANO protocol, sampling, simulation, settings |
| `source/http/`, `include/http/` | Web server, authentication, configuration/file APIs |
| `source/watchdog/`, `include/watchdog/` | Worker heartbeat supervision |
| `html/` | Browser pages, styles, themes, JavaScript |
| `scripts/` | Build, deployment, model/configuration synchronization |

[main.c](source/core/main.c) coordinates startup and shutdown. Camera frames feed inference and annotated video; inference results feed tracking, following, and TXT reports. Encoded video is shared by recording and live clients. Telemetry reads snapshots from the wheel controller.

## Diagnostics

```bash
systemctl status autonomous-driving-car --no-pager
journalctl -u autonomous-driving-car -n 100 --no-pager
curl -s http://127.0.0.1/api/video/stats
curl -s http://127.0.0.1/api/follow
curl -s http://127.0.0.1/api/ano
```

For startup failures, check the model, NPU driver/runtime, and camera availability. For video issues, inspect capture/encoding counters. For missing motor output, inspect following readiness, CAN state, and wheel feedback. Routes are defined in [http.c](source/http/http.c).

The service allows core dumps, and Release builds retain debug information. If the system has a configured `systemd-coredump` collector, inspect crashes with:

```bash
sudo coredumpctl list /opt/autonomous-driving-car/bin/autonomous-driving-car
sudo coredumpctl info /opt/autonomous-driving-car/bin/autonomous-driving-car
```

Use the executable from the matching build when analyzing a dump.

## License

[GPL-3.0-or-later](LICENSE).
