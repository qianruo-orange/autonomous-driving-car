# ROCK 4D：Linux 7.4-rc1 适配

本轮直接使用用户提供的 `linux-master.zip` 解压源码，路径为 `/home/qianruo/kbuild/linux-7.4-rc1-user`。旧的“7.3 源码加官方增量”构建已按要求停止；新构建不复用旧目录的 `.o`、`.ko` 或其他构建产物。

归档的全部文件 CRC 已验证，保留了符号链接及执行权限。ZIP 注释中的提交 `6628e7d4c6b25c8d18f1345e3a20d21829aae7e4` 与 [官方 Linux 7.4-rc1 提交](https://github.com/torvalds/linux/commit/6628e7d4c6b25c8d18f1345e3a20d21829aae7e4) 对应；未独立验证上游签名。ZIP SHA-256：

```text
8b6d02fd5e730d8ebcc09dd241b924e8c50dcfca33512da3774a91836e46a25d
```

原始校验记录在 `/home/qianruo/kbuild/car-kernel-7.4-rc/user-source-verification.json`，源树副本在 `.rock4d/source-verification.json`。

## 适配内容

内核树已应用 Kiln NPU 所需的电源域、IOMMU、设备树适配，以及 RK3576 JPEG 解码驱动和设备树节点。配置继承当前实时内核的功能，启用 PREEMPT_RT；JPEG 驱动作为模块构建。实验性 VEPU510 编码驱动和与现有 RKNN 用户库不兼容的 Rocket 路径不启用。

本目录保存完整构建配置 `config` 和可重放补丁：

| 补丁 | 应用位置与用途 |
| --- | --- |
| `0001-kiln-npu.patch` | 内核根目录；保留 NPU 电源域、复位、时钟、IOMMU 和板级供电适配 |
| `0002-jpeg-dt.patch` | 内核根目录，在 0001 之后；连接 JPEG Kbuild 与设备树，驱动文件取自工程的 `kernel/rockchip-jpegd` |
| `0003-aic8800-cfg80211.patch` | 原始 `aic8800-usb-5.0-kiln` 包根目录；适配 7.4 的密钥类型接口，保持旧版本兼容 |
| `0004-aic8800-parallelism.patch` | AIC 包根目录，在 0003 之后；移除厂商 Makefile 强制覆盖并行度的设置 |

新树 `.rock4d/drivers` 内的源码已经完成外置驱动适配，无需重复应用 0003、0004。Kiln NPU 的 mainline shims 也已包含在其中；DKMS 预构建步骤使用现有源码，不重新下载覆盖。本次通过 Codex 辅助整理和验证，没有添加人的 DCO 签署。

Wi-Fi、蓝牙和 vendor NPU 驱动保留为外置模块，源码随新树放在 `.rock4d/drivers` 中：

| 功能 | 相对于新源树的源码路径 | 构建产物 |
| --- | --- | --- |
| NPU | `.rock4d/drivers/kiln-rknpu-0.9.8` | `rknpu.ko` |
| Wi-Fi 固件加载、网络接口 | `.rock4d/drivers/aic8800-usb-5.0-kiln/USB/driver_fw/drivers/aic8800` | `aic_load_fw.ko`、`aic8800_fdrv.ko` |
| 蓝牙 | `.rock4d/drivers/aic8800-usb-5.0-kiln/USB/driver_fw/drivers/aic_btusb` | `aic_btusb.ko` |

外置模块必须在完整内核构建生成 `Module.symvers` 后，针对同一源树严格编译和链接。仅通过源码接口预检不等于正式模块验证；不能复制 7.3 的 `.ko` 安装到 7.4。

Wi-Fi 与蓝牙的二进制固件沿用本机 `/lib/firmware/aic8800*`，不作为源码参与编译。NPU 沿用现有 RKNN 用户库与模型。安装新内核时必须保留这些文件；本次源码适配不代表固件和用户库已获得主线支持。

## 构建

构建脚本仅处理已经适配并准备好配置的源树，不下载、打补丁、安装软件包或重启。默认使用 `-j24`，包版本为 `7.4~rc1-npu-rt-2`：

```bash
nice -n 10 bash /home/qianruo/autonomous-driving-car/scripts/build_kernel74.sh
```

也可以指定源码和日志目录：

```bash
nice -n 10 bash /home/qianruo/autonomous-driving-car/scripts/build_kernel74.sh \
  /home/qianruo/kbuild/linux-7.4-rc1-user \
  /home/qianruo/kbuild/car-kernel-7.4-user
```

流程依次构建内核 Debian 包、NPU、Wi-Fi 和蓝牙模块，任一步失败即停止，不启用忽略缺失符号的选项。`bindeb-pkg` 可能同时生成 `linux-libc-dev` 头文件包，脚本不会安装它，也不会替换宿主用户态库。

日志目录分别保存 `kernel.log`、`rknpu.log`、`wifi.log`、`bluetooth.log` 和各阶段时间、退出码。总状态见 `build-stage.txt`、`build-started.txt`、`build-finished.txt`、`build-exit-code.txt`；成功后 `artifacts.sha256` 记录 image、headers 包及四个外置模块的校验值。

完整构建于 2026 年 11 月 3 日启动。脚本在严格链接前清理外置模块预检产物，避免把缺少最终符号版本的临时 `.ko` 当成交付文件。完成状态以本轮日志和退出码为准，旧目录 `/home/qianruo/kbuild/car-kernel-7.4-rc` 的日志仅作历史记录。

## 验证边界

打入补丁、设备树编译、驱动接口预检、完整打包和实机启动是不同阶段。本文不提前宣称完整构建或新内核启动成功。安装前还需检查匹配的镜像、设备树、initramfs、外置模块和回退引导；启动后分别验证网络、NPU 推理、JPEG 硬解及实时调度。现有 7.3 内核和启动配置继续保留。
