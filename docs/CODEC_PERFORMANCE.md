# RK3576 软硬编解码验证与性能

日期：2026 年 11 月 3 日，Asia/Shanghai。设备：Radxa ROCK 4D / RK3576；测试运行内核 `7.3.0-npu-rt`。摄像头维持 1280×720 MJPEG、请求 30 FPS。以下数据来自本机实际调用与码流检查。

## 当前结果

JPEG 硬件解码已在本机验证并接入工程。采用上游待审的 `rkjpegd` v7，增加 RK3576 兼容项、硬件版本校验及设备树资源；这是主线内核上的外置适配。它与用于 H.264/HEVC 的 RKVDEC、DRM/Vulkan Video 编码探索是不同的路径。

网页只保留相机设备、分辨率与帧率设置，不再提供视频编码/解码后端选项；公开配置接口也不接受旧的后端选择字段。程序检测并优先使用已安装的 JPEG 硬解，未安装驱动的环境明确报告软件解码。录像固定使用已验证的 libx264；软件编码器初始化失败会报错，不会误切到尚不稳定的实验编码器。

## JPEG 解码对照

输入为 8 张独立保存的、未画框的实际相机 JPEG。相同线程依次对照硬件和软件路径，每张预热 2 次后各测 10 次，共 80 组。暂停在线服务和内核构建，固定调用线程为 CPU4、FIFO 10。比较范围为压缩 JPEG 到原尺寸 RGB，包含硬件队列、等待、缓存同步和颜色转换，未包含摄像头交付、推理、渲染与视频编码。

| 指标，均值/帧 | libjpeg-turbo 软件解码 | JPEG 硬解＋RGB 转换 | 变化 |
| --- | ---: | ---: | ---: |
| 墙钟耗时 | 5.288 ms | 2.321 ms | 减少 56.12% |
| 调用线程 CPU 时间 | 5.263 ms | 1.496 ms | 减少 71.57% |

CPU 时间来自 `CLOCK_THREAD_CPUTIME_ID`，不包含中断或内核工作线程的全部系统开销。不能直接把它等同于整机 CPU 占用。未锁定温度与频率，此结果是固定小样本的短时比较，不是长期上界。

硬解会输出缓存 DMA-BUF 中的 NV12；CPU 访问前后使用 `DMA_BUF_IOCTL_SYNC`，随后用全范围 BT.601 NEON 转成 RGB。直接在设备 MMAP 内存上做通用颜色转换曾明显变慢，最终实现避免了这条路径，也没有额外解码线程。

与软件 RGB 对照，平均每通道绝对差约 0.0646，PSNR 56.47dB，最大单通道差 29。JPEG 的 IDCT、色度处理和取整路径不同，因此不宣称逐像素完全一致。验证包括 99 次有效解码、分辨率切换、纯红颜色检查，以及两次刻意触发的截断/尺寸不符错误后的恢复。另用 ASan/UBSan 检查了 448335 个 RGB 分量，覆盖奇数尺寸、行步长、SIMD 尾部与输出边界。

## H.264 编码对照及限制

生产软件路径使用 NV12、1280×720、30 FPS、目标码率 1612800 bit/s、GOP 60、无 B 帧、`veryfast`、`zerolatency`。在暂停在线服务的条件下，将调用线程和继承的 libx264 辅助线程限制到 CPU4–7；输入为上述 8 张实际相机图像转成的同一组 NV12，循环编码 80 帧，统计排除前 5 帧的预热。

| 路径 | 平均调用耗时 | 平均进程 CPU 时间/帧 | 有效性 |
| --- | ---: | ---: | --- |
| 软件 libx264，GOP 60 | 13.030 ms | 40.089 ms | 80/80 帧，ffprobe 确认 1280×720 H.264，完整解码通过 |
| 软件 libx264，全 I 帧诊断 | 8.392 ms | 24.488 ms | 80 次调用成功；仅用于对照实验，不改变生产 GOP |
| VEPU510 实验驱动 | 无有效稳定均值 | 未作为成功结果统计 | 连续编码出现空帧/错误，不能替换生产后端 |

进程 CPU 时间包括软件编码辅助线程，多线程 CPU 时间可以大于墙钟耗时。13.030ms 是当前输入下的串行调用成本，不是完整直播帧率或编码质量等价的保证。

本机已有 2026 年 7 月的 VEPU510 V4L2 RFC 驱动，显式加载后生成两个编码节点。1280×720 合成输入的 3 帧测试得到 2856 字节 I 帧、274 字节 P 帧及 0 字节错误帧，进程返回失败；真实图像的全 I 帧对照也在首帧失败，耗时约 816.6ms、输出 0 字节。这个失败等待不能当作硬件编码性能数值，更不能据此声称硬编更快或更慢。

上游作者与维护者已讨论转向 DRM/Vulkan Video、内核与 Mesa 分层的编码方案，仍不能把 RK3588 的开发进展直接视为 RK3576 VEPU510 可用。当前没有找到能在本机稳定完成同参数连续编码、可交付的更新补丁。因此保留软件编码，实验驱动禁止自动加载，仍可按名称显式加载进行隔离验证。

## 安装、状态与证据

安装方法见独立内核仓库的 [JPEG 驱动说明](https://github.com/qianruo-orange/rock-4d-npu-kernel/tree/main/rockchip-jpegd)。模块匹配当前内核构建；设备树在备份后原子更新。JPEG 首选视频节点编号 32，避免占用摄像头低编号节点；应用按驱动身份发现，未写死节点号。

`/api/camera` 只读报告实际选用的解码后端、硬解帧数和解码错误数。上线核验同时检查采集、显示、编码计数增长、录像写入、所有工作线程 FIFO 以及车辆跟随关闭。已有模型及相机分辨率不因解码器安装而改变。

原始资料位于 `/home/qianruo/kbuild/car-jpeg-hw/`，不纳入仓库测试目录：

- `decoder-cpu-isolated.json`、`decoder-cpu-isolated.log`：最终软硬解及调用线程 CPU 对照。
- `decoder-neon-isolated.json`、`check-color.c`：颜色、恢复与边界检查。
- `encode-sw-gop60.log`、`sw-encode-validation.json`、`sw-encode-decode-check.log`：生产软件编码参数与完整码流验证。
- `encode-sw-gop1.log`、`encode-hw-gop1.log`、`vepu510-test.log`：编码诊断与失败证据。
- `install-final.log`、`deploy-verification.json`：模块安装和应用部署验收。

上游参考：[rkjpegd v7](https://lore.kernel.org/all/20261022-rockchip-jpegdec-v7-0-3fd7dd55f408@pengutronix.de/)、[VEPU510 RFC 与维护者审查](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-July/1152929.html)、[作者的 DRM/Vulkan Video 方向说明](https://lists.infradead.org/pipermail/linux-arm-kernel/2026-July/1153737.html)。
