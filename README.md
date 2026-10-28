# Autonomous Driving Car

A Linux vehicle project combining camera capture, RKNN YOLO26 detection, differential target following, SocketCAN, and a browser management interface. The application uses C11/C++11; the frontend uses HTML, CSS, and JavaScript; build and deployment tools use Bash.

The hardware validation platform is **Radxa Rock 4D / RK3576**. Actual motion requires a compatible motor protocol and measured wheel feedback. The supplied motor profile is a disabled example.

## Features

- Camera selection, resolution/frame-rate configuration, continuous autofocus where supported, and annotated live video.
- YOLO26 inference with adaptive worker allocation and frame-matched detection results.
- Intelligent following with target selection, automatic/manual speed, steering PD, and independent left/right wheel PI control.
- Shared CAN/CAN FD communication, DBC decoding, interface configuration, and motor command/feedback integration.
- H.264/MSE and MJPEG streaming, annotated MP4 recordings, daily detection TXT files, and file downloads/cleanup.
- Web monitoring and configuration, CPU policies, network diagnostics, and Anonymous Assistant telemetry.

## Requirements

Use Linux with V4L2, the Rockchip NPU driver, and a board-compatible `librknnrt.so`. CAN features additionally require a SocketCAN interface.

Build dependencies:

- C11/C++11 compiler, GNU Make, CMake **3.18+**, Bash, and pkg-config.
- libsystemd, libnl-3/libnl-route-3, libjpeg, and OpenCV core/imgproc development libraries.
- Standard Linux tools including `flock`, `sha256sum`, `strings`, `awk`, `tar`, and `zip`.
- FFmpeg libavcodec/libavutil are optional at build time; software H.264 encoding uses FFmpeg's `libx264` encoder. Hardware encoding depends on the available RKMPP/V4L2 backend.

Provide a compatible **YOLO26 RKNN model** at `config/yolo26.rknn` and class labels at `config/coco.names`. The application requires working inference to start normally.

## Build and deploy

Run all builds and deployments through the repository scripts:

```bash
bash scripts/build.sh -R
sudo bash scripts/deploy.sh -i
```

The executable is written to `bin/autonomous-driving-car`. Deployment installs it and the frontend under `/opt/autonomous-driving-car`, installs the systemd unit, and starts the service.

| Command | Purpose |
| --- | --- |
| `bash scripts/build.sh -R` | Release build |
| `bash scripts/build.sh -D` | Debug build |
| `bash scripts/build.sh -R -P rk3576` | Specify the model platform instead of detecting it |
| `bash scripts/build.sh -C` | Clean build outputs and project logs; does not build |
| `sudo bash scripts/deploy.sh -i` | Install or update the existing deployment |

Use `deploy.sh -i` for normal updates. It preserves existing logs, recordings, and device settings, and reconciles model changes before replacement. The deployed `config.txt` and existing following, motor, CPU, and telemetry settings take precedence; ordinary files such as labels and DBC databases can be replaced by source versions. The service restarts during deployment.

`deploy.sh -u` uninstalls the application; `-r` uninstalls before reinstalling. Both remove the installation directory, including its configuration and recordings.

```bash
systemctl status autonomous-driving-car --no-pager
journalctl -u autonomous-driving-car -f
```

The systemd service runs from `/opt/autonomous-driving-car`, uses a watchdog, and restarts on failure. For foreground debugging, stop the service first and run `sudo ./bin/autonomous-driving-car` from the repository root.

## Web interface

Open `http://<board-ip>/` using the board's current address.

| Page | Address | Contents |
| --- | --- | --- |
| Home | `/` | Annotated video and resource monitoring |
| System Configuration / 系统配置 | `/config` | Camera, YOLO, CAN, Web/telemetry, CPU, and following settings |
| Driving Logs / 行驶日志 | `/logs` | Logs, recordings, detection results, downloads, and cleanup |
| DBC viewer | `/dbc` | CAN signal definitions and decoded data |

HTTP uses fixed port **80**. The home page accepts a local system account; administrative pages and writes require `root` or a `sudo` group account. Authentication uses HTTP Basic.

**Web与匿名助手设置** combines interface IP configuration and telemetry settings with one Save button. Web IP settings manage the selected system interface; HTTP still listens on the board's IPv4 interfaces. Changing static/DHCP addressing can require reconnecting the browser.

Save applies telemetry first, then changed Web IP settings. A failed telemetry save prevents the IP change; an IP-save failure does not roll back telemetry already saved.

Web allows **two client IPv4 addresses**. One address can use multiple page, API, video, or download connections. Its slot is released 30 seconds after its last connection closes; a third address receives HTTP 503. Devices sharing a NAT address share a slot.

## Camera and models

Select camera modes from the device capabilities in System Configuration. Resolution and frame rate are configurable; the driver determines the effective negotiated mode. Capture decoding uses software, while H.264 encoding can use software or available hardware.

`config/yolo26.rknn` is the canonical model path for uploads, builds, and deployment. Upload model/label files from the YOLO section. Models must match the board's NPU platform and supported YOLO26 output layout.

[Model synchronization](scripts/model_sync.sh) compares source and deployed model hashes so an uploaded model can be retained. Independently changed models produce a conflict instead of a silent overwrite. Internal synchronization files live directly under `config/`; the old `config/models/` directory is not created.

H.264 live video and MP4 recording share an encoding session. Stopping recording can also end that H.264 stream. Pipeline stages favor recent frames; recording and display need not include every captured frame.

## Intelligent following

**智能跟随** selects a detected target and controls differential wheel speed. Automatic speed uses relative target size; manual mode uses the configured speed setpoint. Steering follows horizontal target error. Each wheel uses its own PI state and measured speed feedback.

Motor output requires:

- A valid, enabled `config/car_motor_can.conf` profile.
- An enabled CAN interface, fresh detections, and fresh measured left/right wheel speeds.
- Successful `SCHED_FIFO` scheduling at priority **20**.

Following starts stopped after each service restart. Target loss, stale input/feedback, and unavailable prerequisites inhibit motion. `GET /api/follow` reports readiness and motor status. FIFO admission and PREEMPT_RT kernel support are reported separately.

Motor traffic uses the same [CAN module](source/can/can_socket.c) as other CAN communication. Adapt the disabled [example profile](config/car_motor_can.example.conf) to the drive manufacturer's command, stop, and feedback formats. A drive needing a special enable sequence or checksum requires a corresponding adapter.

Wheel targets and steering correction use **m/s**. Target size is a relative visual cue rather than calibrated distance. LLM integration and navigation/path planning are not implemented.

## Anonymous Assistant

Official website: [ANO Tech / 匿名科创](https://www.anotc.com/). Downloads and documentation: [Anonymous Assistant official repository](https://gitee.com/anotc/AnoAssistant).

Supports **Anonymous Assistant 2.0.1** with **ANO V8 over TCP**: board as server, PC as client. The configurable port defaults to **8080**; telemetry is disabled until enabled or restored from saved settings. Configure it under **Web与匿名助手设置**.

Frame `0xF1` carries four `Float32` fields: target speed, actual speed, target steering correction, and actual steering correction. Send rate and simulated waveforms are configurable. Telemetry supports **two TCP clients** and does not issue motor commands.

## Configuration and data

Paths are relative to the runtime working directory. The installed service uses `/opt/autonomous-driving-car`.

| Path | Purpose |
| --- | --- |
| `config/config.txt` | Camera, YOLO, CAN, and Web IP configuration |
| `config/follow.conf` | Following parameters |
| `config/car_motor_can.conf` | Device-specific motor protocol |
| `config/cpu_freq.conf` | CPU policy settings |
| `config/ano.conf` | Anonymous Assistant settings |
| `config/yolo26.rknn` | Runtime model |
| `config/coco.names`, `config/*.dbc` | Class labels and CAN databases |
| `logs/YYYYMMDD/` | Information and error logs |
| `recordings/YYYYMMDD/` | Annotated MP4 recordings |
| `recordings/detections/YYYYMMDD/result_YYYYMMDD.txt` | Daily inference results |

Completed inference results include frames with no targets. TXT entries contain capture time, image size, lens position when available, and detections with confidence and `X1 X2 Y1 Y2` coordinates in the original image.

Configuration downloads include actual configuration files and the model. Empty directories and internal synchronization metadata are omitted; the uncompressed file limit is **64 MiB**.

Driving Logs supports previews, individual downloads, and a whole-page ZIP. Bulk deletion keeps active/finalizing recordings while removing inactive managed files, including interrupted videos. The whole-page ZIP excludes active or incomplete videos.

## Source layout

| Directory | Responsibility |
| --- | --- |
| `source/core/`, `include/core/` | Startup, shared context, configuration, logging, CPU affinity |
| `source/video/`, `include/video/` | Capture, frame ownership, encoding, streaming, recording |
| `source/yolo/`, `include/yolo/` | RKNN inference, drawing, result reports |
| `source/control/`, `include/control/` | Following and wheel control |
| `source/can/`, `include/can/` | Shared CAN sockets, queues, DBC and motor integration |
| `source/tcp/`, `include/tcp/` | Telemetry transport and interface IP helpers |
| `source/telemetry/`, `include/telemetry/` | ANO V8 frames, sampling, simulation, configuration |
| `source/http/`, `include/http/` | HTTP reactor, authentication, configuration and file APIs |
| `source/watchdog/`, `include/watchdog/` | Worker heartbeat supervision |
| `html/` | Browser pages, themes, and JavaScript modules |
| `scripts/` | Shell build, deployment, and model synchronization |

The [entry point](source/core/main.c) coordinates module startup and shutdown. Camera frames feed inference and rendering; completed results feed following control and TXT reports. Encoded video serves both recording and browser clients. Telemetry reads controller snapshots through the TCP worker.

## Diagnostics

```bash
systemctl status autonomous-driving-car --no-pager
journalctl -u autonomous-driving-car -n 100 --no-pager
curl -s http://127.0.0.1/api/config
curl -s http://127.0.0.1/api/follow
curl -s http://127.0.0.1/api/ano
curl -s http://127.0.0.1/api/video/stats
```

Check model/platform compatibility for inference startup failures, capture/encoding counters for video issues, and controller readiness/feedback for missing motor output. Detailed API routes are defined in [http.c](source/http/http.c).

## License

[GPL-3.0-or-later](LICENSE).
