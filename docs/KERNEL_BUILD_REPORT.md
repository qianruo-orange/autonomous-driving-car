# RK3576 内核编译、安装与实时调度验证报告

文档日期：2026 年 11 月 2 日（Asia/Shanghai）  
工程：autonomous-driving-car  
设备：Radxa ROCK 4D，Rockchip RK3576，ARM64  
范围：已完成的 Linux 7.3.0-npu-rt 构建、Kiln NPU 适配、安装启动与应用实时调度验证。

## 1. 交付结果

此前内核已编译、打包、安装并切换启动。运行系统的 `uname -a` 显示 `7.3.0-npu-rt` 和 `SMP PREEMPT_RT`，运行内核的 `/proc/config.gz` 确认 `CONFIG_PREEMPT_RT=y`。本报告生成时没有重新编译内核、安装软件包或重启设备。

构建目录为 `/home/qianruo/kbuild/linux-7.3-npu-rt`。最终内核包的 `.buildinfo` 记录 `MAKEFLAGS="rR -j24 ... KDEB_PKGVERSION=7.3-npu-rt-1"`，因此最终构建并行度确认为 **24**。其中 `DEB_BUILD_OPTIONS="parallel=1"` 是打包环境记录，应与 make 的实际 `-j24` 区分；不能据此把本次内核编译误写成单任务。

安装目录下有 **3478 个内核树模块**，另有 **4 个 DKMS 模块**，合计 3482 个已安装模块文件。3478 来自 `/lib/modules/7.3.0-npu-rt/kernel` 的独立计数，并与构建日志中的 3478 条唯一 `INSTALL ... .ko` 路径一致。构建工作树的递归 `.ko` 数量会包含测试、辅助产物和打包副本，不用作安装进度或完成比例。

## 2. 来源与构建环境

| 项目 | 本次留存的证据 |
| --- | --- |
| 内核版本 | 源树 Makefile：VERSION=7、PATCHLEVEL=3、SUBLEVEL=0 |
| 本地源码归档 | `/home/qianruo/kbuild/linux-7.3.tar.xz`，162399360 字节 |
| NPU 补丁 | `/home/qianruo/kbuild/npu-patches/0001` 至 `0010`，附原始 README |
| 完整构建配置 | `/home/qianruo/kbuild/config-7.3.0-npu-rt` |
| 构建配置差异 | `/home/qianruo/kbuild/preempt-rt-config.diff` |
| 构建日志 | `/home/qianruo/kbuild/preempt-rt-build.log`，结束标记 `RT_BUILD_COMPLETE` |
| 打包记录 | `linux-upstream_7.3-npu-rt-1_arm64.buildinfo` 与 `.changes` |
| 构建工具 | GCC 14.2.0、binutils 2.44、GNU make 4.4.1、dpkg 1.22.22 |
| 构建架构 | 本机 ARM64 原生构建 |
| Debian 打包兼容 | 构建树 `scripts/package/mkdebian` 使用 debhelper-compat 13 |

源码树没有 `.git` 元数据，报告以留存源码归档、补丁、配置和包校验值作为追溯依据，不虚构上游 Git 提交号。本地归档校验值用于确认本次文件身份，未在本报告中独立验证上游签名。

Linux 7.3 源码内已有实时抢占配置；本次启用 `CONFIG_PREEMPT_RT`。现有构建证据未显示另行叠加外部 `patch-*-rt*.patch`。Kiln 补丁解决 RK3576 的 NPU 电源域、IOMMU 和设备树适配，与启用实时抢占是两件不同的工作。

## 3. Kiln 补丁及实际适配

本内核使用了 Kiln 的 RK3576 NPU 补丁。原始补丁说明指出，其目标是让主线风格内核能够配合外置 vendor `rknpu` 驱动运行 NPU。仅安装驱动模块不足以替代内置电源域与 IOMMU 修改。

| 编号 | 涉及位置 | 解决的问题 |
| --- | --- | --- |
| 0001 | Rockchip 电源域 | NPUTOP/NPU0/NPU1 退出空闲后延迟 15µs，再恢复 QoS |
| 0002 | Rockchip 电源域 | 上电时执行设备树提供的域复位脉冲 |
| 0003 | Rockchip IOMMU | 获取设备树列出的全部时钟，覆盖 NPU 的额外门控 |
| 0004 | RK3576/ROCK 4D 设备树 | 加入 vendor rknpu 节点及对应 IOMMU |
| 0005 | RK3576 设备树 | 补齐 NPU 电源域时钟与 BIU reset |
| 0006 | Rockchip 电源域 | 处理热上电路径中可选 SRAM reset 的超时 |
| 0007、0008 | Rockchip IOMMU | 跳过固件留下、已空闲的孤立 fault bank，避免 stall 等待卡住 |
| 0009 | Rockchip 电源域 | 在上电路径触及 NPU 核心与 MMU 寄存器，完成冷启动初始化 |
| 0010 | ROCK 4D 设备树 | 为 NPU 电源轨增加 `regulator-always-on`，避免重复推理时访问断电设备 |

原始 README 主要描述 7.1.3 的适配。对当前 7.3 源树执行只读反向补丁检查，0002、0004 至 0010 能完整识别；0001 与 0003 有后续修改造成的上下文差异，但源文件中可直接确认 15µs 延迟、NPU 域配置和 `devm_clk_bulk_get_all()` 的实际实现。0003 对零时钟情况采用等价的 `else` 赋值为 0 分支。因此不能把本次过程描述成十个原始补丁在 7.3 上均无差异直接应用。

原始补丁说明对若干实现仍有上游审查意见，例如经验性延迟、设备树绑定和寄存器初始化方式。报告记录本机验证结果，不把这些外置补丁宣称为已上游合入。

## 4. 实时内核配置

| 配置 | 值 | 含义 |
| --- | --- | --- |
| CONFIG_LOCALVERSION | `-npu-rt` | 区分原 `-npu` 内核与模块 ABI |
| CONFIG_PREEMPT_RT | y | 启用实时抢占 |
| CONFIG_IKCONFIG / CONFIG_IKCONFIG_PROC | y / y | 可读取实际运行内核配置 |
| CONFIG_RT_GROUP_SCHED | y | 启用实时组调度，需处理 cgroup 限制 |
| CONFIG_RCU_BOOST | y | 启用 RCU 优先级提升 |
| CONFIG_HZ | 250 | 保留本次实际配置；并非 1000Hz |
| CONFIG_ROCKCHIP_PM_DOMAINS | y | NPU 所需电源域实现为内置驱动 |
| CONFIG_ROCKCHIP_IOMMU | y | IOMMU 为内置驱动 |
| CONFIG_DRM_ACCEL_ROCKET | 未启用 | 本次采用外置 vendor rknpu 路径 |
| CONFIG_MODULE_SIG | 未启用 | 本次 DKMS 模块未签名 |

启用 PREEMPT_RT 后，`olddefconfig` 会联动调整部分配置；完整差异以 `preempt-rt-config.diff` 为准。不能仅复制三个开关而假定两份内核配置完全等价。

## 5. 构建产物与时间线

| 时间（北京时间） | 事件与证据 |
| --- | --- |
| 11 月 1 日 17:21 | 留存 RT 配置与差异文件 |
| 11 月 1 日 23:45 | Debian changelog 记录打包版本 `7.3-npu-rt-1` |
| 11 月 1 日 23:47 | 运行内核版本中记录的编译时间 |
| 11 月 1 日 23:50 | 构建树 `vmlinux` 与 ARM64 Image 完成 |
| 11 月 2 日 00:14 | `.buildinfo` 记录二进制包完成；构建日志结束 |
| 11 月 2 日 00:25 | 第一次安装尝试，检测镜像比较失败后恢复原引导链接 |
| 11 月 2 日 00:30 | 第二次安装验证成功，切换 Image、uInitrd 和 dtb 链接 |
| 11 月 2 日 00:31 | 留存重启请求时间；后续开机检查确认进入 RT 内核 |

这段时间线来自产物和日志。没有可靠的构建启动时间记录，因此不把配置文件时间当作编译开始时间，也不据此计算准确总耗时。

产物均位于 `/home/qianruo/kbuild/`：

| 文件 | 大小（字节） | 用途 |
| --- | ---: | --- |
| linux-image-7.3.0-npu-rt_7.3-npu-rt-1_arm64.deb | 53722952 | 内核镜像与内核树模块 |
| linux-headers-7.3.0-npu-rt_7.3-npu-rt-1_arm64.deb | 10209220 | 匹配内核的头文件，供 DKMS 构建 |
| linux-libc-dev_7.3-npu-rt-1_arm64.deb | 1493012 | 构建产生的用户态头文件包；本次安装日志未显示安装该包 |
| linux-7.3-npu-rt/arch/arm64/boot/Image | 38783488 | 供本机 U-Boot 使用的未压缩 ARM64 镜像 |

## 6. 安装与启动中的修正

第一次安装已完成 image/headers 解包、NPU 与无线 DKMS 构建，但把构建树未压缩 `Image` 与包安装的压缩 `vmlinuz` 直接比较，出现“第 1 字节不同”。安装保护逻辑随即恢复原引导链接，保留现场，未把该次比较失败当作成功。

第二次安装按压缩格式处理镜像，验证解压后的 38783488 字节 ARM64 Image，再生成 initramfs、安装匹配 ROCK 4D 的设备树并切换链接。当前链接为：

```text
/boot/Image   -> Image-7.3.0-npu-rt
/boot/uInitrd -> uInitrd-7.3.0-npu-rt
/boot/dtb     -> dtb-7.3.0-npu-rt
```

`/boot/armbianEnv.txt` 的 `fdtfile` 为 `rockchip/rk3576-rock-4d.dtb`。Image、设备树和模块必须匹配，不能仅更换内核文件而继续使用旧设备树。

DKMS 记录与模块 `vermagic` 均确认 RT 内核 ABI：`7.3.0-npu-rt SMP preempt_rt mod_unload aarch64`。安装的模块为 `rknpu`（kiln-rknpu/0.9.8）、`aic_load_fw_usb`、`aic8800_fdrv_usb` 和 `aic_btusb_usb`（后三者来自 aic8800-usb/5.0-kiln）。加载后的模块名称分别为 rknpu、aic_load_fw、aic8800_fdrv、aic_btusb。

## 7. 复现构建与安装

以下命令针对本机已保存、已适配的 RT 源树，不自动下载或覆盖源码。应先备份现有配置，再使用报告中的校验值确认输入。若从干净 7.3 归档重建，需要先移植并检查上节列出的 Kiln 修改；原始 7.1.3 补丁不能未经检查就视作适用于所有 7.3 源树。

```bash
cd /home/qianruo/kbuild/linux-7.3-npu-rt
cp /home/qianruo/kbuild/config-7.3.0-npu-rt .config
make olddefconfig
make -j24 KDEB_PKGVERSION=7.3-npu-rt-1 bindeb-pkg \
  > /home/qianruo/kbuild/preempt-rt-build-rebuild.log 2>&1
```

构建返回成功后，先检查 `make kernelrelease`、三个 Debian 包和 `.buildinfo`，再安装匹配的 headers 与 image。下面是本次安装流程的核心步骤，**不能替代对引导备份、设备树与 DKMS 的验证**：

```bash
cd /home/qianruo/kbuild
sudo dpkg -i \
  linux-headers-7.3.0-npu-rt_7.3-npu-rt-1_arm64.deb \
  linux-image-7.3.0-npu-rt_7.3-npu-rt-1_arm64.deb
sudo /usr/sbin/dkms autoinstall -k 7.3.0-npu-rt
sudo /usr/sbin/depmod -a 7.3.0-npu-rt
sudo update-initramfs -u -k 7.3.0-npu-rt
```

本机 Image 特别处理：确认 `/boot/vmlinuz-7.3.0-npu-rt` 的压缩格式，将其解压到临时文件，与构建树 `arch/arm64/boot/Image` 比较，通过后原子安装成 `/boot/Image-7.3.0-npu-rt`。将构建树 ROCK 4D 设备树放入版本化 dtb 目录，核对模块 ABI 与 uInitrd 后，再原子切换三个链接。此文档生成操作不会执行上述安装或再次重启。

## 8. 应用实时调度与重启兼容

应用在 `source/core/main.c` 的同一模块表中管理初始化、销毁、线程入口、调度策略、优先级和 CPU 分组。HTTP 管理自己的内部线程，YOLO 管理推理子线程；创建属性读取 main 中的对应表行。

| 角色 | SCHED_FIFO 优先级 | CPU 分组 |
| --- | ---: | --- |
| main、TCP、HTTP reactor | 5 | LITTLE |
| HTTP worker、推理日报 | 1 | LITTLE |
| 解码、渲染、录像与编码辅助线程 | 10 | BIG |
| NPU 推理 worker | 12 | BIG |
| 摄像头采集 | 15 | BIG |
| CAN 收发 | 15 | LITTLE |
| 跟随控制 | 20 | LITTLE |
| 看门狗 | 30 | LITTLE |

CPU 分组根据能力或频率发现，本机 LITTLE 为 CPU0–3、BIG 为 CPU4–7；源码不将编号作为固定适配前提。实时策略属于应用线程属性，安装 RT 内核本身不会自动把全部用户程序改成实时线程。

曾复现服务首次开机成功、重启时申请 FIFO 返回 `EPERM`。本机启用了 `CONFIG_RT_GROUP_SCHED`，cgroup v2 的 CPU 控制器会使新实时任务受到组预算约束；单纯增加 root 权限不能解决。当前主机配置为：

```ini
# /etc/systemd/system/-.slice.d/90-autonomous-driving-car-rt.conf
[Slice]
DisableControllers=cpu
```

这是整个主机 cgroup 层级的设置，会影响 CPU 权重与配额。部署脚本在停止旧服务前验证兼容条件。本机全局实时周期和预算均为 1000000µs，未改成无限制；内存与进程数控制器继续工作。细节与适用条件见仓库 `docs/KERNEL_VERIFICATION.md` 以及 Linux cgroup v2、systemd resource-control 官方文档。

## 9. 验证结果与解释

启动检查服务 `rock4d-rt-boot-check.service` 读取实际运行配置，确认 RT 内核、四个加载模块、应用服务、推理后显示和编码帧增长，输出 `/var/lib/rock4d-rt/boot-check.json`。缺少旧的 `/sys/kernel/realtime` 接口不表示 PREEMPT_RT 失效，本机以配置和运行版本交叉判断。

既有调度专项验证逐一检查常驻线程与 HTTP 工作线程；并发视频期间观察到 30 个线程，策略、角色优先级和 CPU 分组符合模块表。应用功能验证覆盖登录、状态接口、原始和标注 MJPEG、fMP4、模型重载、录像重建与停止/启动。原始结果留存于 `/var/lib/rock4d-rt/main-scheduling-checks.json`。

在实时应用及并发视频请求期间，原生定时测试绑定 CPU1、FIFO 10、锁定内存，执行 10000 次、间隔 1ms 的 CLOCK_MONOTONIC 绝对等待：

| 指标 | 实测延迟 |
| --- | ---: |
| 平均 | 34.689µs |
| P50 | 31.847µs |
| P95 | 72.191µs |
| P99 | 134.020µs |
| 最大 | 638.341µs |
| 大于或等于 1ms | 0 次 |

数据来自 `/var/lib/rock4d-rt/main-scheduling-latency.json`。这是约 10 秒的调度唤醒测试，不能当作最坏时延证明，也不能当作摄像头到网页显示的端到端延迟。板卡在持续高负载下曾出现热限频，性能比较需同时记录温度、频率和业务负载。

## 10. 回退与验证边界

成功安装前的引导备份保存在 `/var/backups/rock4d-kernel-20261102-003017`，路径亦记录于 `/var/lib/rock4d-rt/boot-backup-path`。旧的 7.3.0-npu 与 7.2.8-edge-rockchip64 镜像、initramfs 和模块仍保留。

回退应从备份恢复三个原始引导链接及必要的启动配置，确认旧 Image、uInitrd、dtb 和对应模块实际存在后再重启。成功安装日志记载的原链接为 `Image -> vmlinuz-7.3.0-npu`、`uInitrd -> uInitrd-7.3.0-npu`、`dtb -> dtb-7.2.8-edge-rockchip64`；这是当时的备份现场，执行恢复时应以备份内容为准，不将这个历史组合当成通用推荐配置。设备无法启动时，应通过串口或离线挂载启动介质恢复备份。

本次验证没有配置 CAN 或电机驱动，也没有启用实际车辆跟随；总线、电机闭环尚未验证。软件编解码运行正常不代表硬件编解码已通过；蓝牙模块加载不代表配对、音频或数据传输已验证。既有 UFS、设备链接等启动警告不属于本次已解决的问题。

## 11. 校验值

以下为本次留存文件的 SHA-256。完整字符串用于复制核验，不因排版换行而改变。

```text
linux-7.3.tar.xz
87363c24f10e06321eb362765521094e94471f6bff8bce2d6b3e40f39f004953

config-7.3.0-npu-rt
b5b6558493ab7cca1b3516011ade3f60d9a921c8afd9f5d844ff22969690a3d0

linux-image-7.3.0-npu-rt_7.3-npu-rt-1_arm64.deb
4d9a975b128b6680d57cb1d0117661e89993a97dca922291c179c3a9cba7eb9b

linux-headers-7.3.0-npu-rt_7.3-npu-rt-1_arm64.deb
fffa4e4bcb2717a27785e6bd84cc8dc1bac112d6d3e110b7d816ec56cb4b48c5

linux-libc-dev_7.3-npu-rt-1_arm64.deb
c8a1027fe38fb1967de55806fead4caaff7f31cbfdd7b7bfaaf744a3b61f94be
```

## 12. 证据索引与参考

| 位置 | 内容 |
| --- | --- |
| `docs/KERNEL_VERIFICATION.md` | 仓库内的实时调度专项验证记录 |
| `/home/qianruo/kbuild/preempt-rt-build.log` | 内核编译、3478 个模块安装与打包完成 |
| `/home/qianruo/kbuild/preempt-rt-install.log` | 两次安装、失败回滚、镜像修正、DKMS 与引导切换 |
| `/home/qianruo/kbuild/npu-patches/README.md` | Kiln 补丁原始说明与已知上游审查问题 |
| `/home/qianruo/kbuild/linux-upstream_7.3-npu-rt-1_arm64.buildinfo` | 工具版本、-j24 与包校验值 |
| `/var/lib/rock4d-rt/boot-check.json` | 开机后内核与服务检查结果，会随检查重新生成 |
| `/var/lib/rock4d-rt/main-scheduling-checks.json` | 应用调度专项检查 |
| `/var/lib/rock4d-rt/main-scheduling-latency.json` | 10000 次定时唤醒原始统计 |

官方背景资料：[Linux cgroup v2 文档](https://docs.kernel.org/admin-guide/cgroup-v2.html)、[systemd 资源控制](https://www.freedesktop.org/software/systemd/man/latest/systemd.resource-control.html)。本报告的本机结果依据上述留存文件与只读检查，不依赖网页对本设备状态的推断。
