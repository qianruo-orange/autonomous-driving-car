# RK3576 JPEG 硬件解码

此目录保留 Sascha Hauer 的 `rkjpegd` v7 驱动，补上本机 RK3576 适配。它运行在主线内核上，是尚未合入主线的外置补丁，不能称为原生主线支持。

上游来源：[2026-10-22 v7 系列](https://lore.kernel.org/all/20261022-rockchip-jpegdec-v7-0-3fd7dd55f408@pengutronix.de/)。原版权与 GPL-2.0 标记保留。上游支持 RK3568、RK3588；本次新增 `rockchip,rk3576-jpegd`，并在该兼容项下校验本机实际 JPEG 硬件版本 `0xdb1f0006`。寄存器、时钟、复位、IRQ 和电源域与 Rockchip 官方 RK3576 设备树交叉核对。

驱动接受独立 JPEG/MJPEG 帧，输出 NV12 或 GREY。应用目前使用 NV12，采用缓存 DMA-BUF、显式 CPU 同步以及全范围 BT.601 到 RGB 的 NEON 转换。新增驱动不更改 USB 摄像头的输出格式。

安装到本机当前运行内核：

```bash
sudo bash scripts/install_jpeg_decoder.sh
bash scripts/build.sh -R
sudo bash scripts/deploy.sh -i
```

需要当前内核匹配的 headers、构建工具、device-tree-compiler 和 kmod。脚本仅面向已验证的 Radxa ROCK 4D；先构建并生成候选设备树，再备份、安装模块与验证绑定。启动设备树在成功后原子替换。当前启动缺少 JPEG 节点时，通过临时 OF overlay 模块激活；下次启动直接使用已补齐节点的设备树。

备份保存在 `/var/backups/car-jpeg-decoder-*`。回退时先停止应用，恢复备份中的 board.dtb 至记录的 dtb-path，移除 `/etc/modules-load.d/autonomous-driving-car-jpeg.conf` 后重启；没有 JPEG 设备时应用自动选择软件解码。保留旧内核及匹配模块。不同内核版本必须重新构建驱动，不能复制旧 `.ko` 使用。
