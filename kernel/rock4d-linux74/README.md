# ROCK 4D：Linux 7.4-rc1 适配

本轮直接使用用户提供的 `linux-master.zip` 解压源码，路径为 `/home/qianruo/kbuild/kernels/7.4-rc1/source`。旧的“7.3 源码加官方增量”构建已按要求停止；新构建不复用旧目录的 `.o`、`.ko` 或其他构建产物。

归档的全部文件 CRC 已验证，保留了符号链接及执行权限。ZIP 注释中的提交 `6628e7d4c6b25c8d18f1345e3a20d21829aae7e4` 与 [官方 Linux 7.4-rc1 提交](https://github.com/torvalds/linux/commit/6628e7d4c6b25c8d18f1345e3a20d21829aae7e4) 对应；未独立验证上游签名。ZIP SHA-256：

```text
8b6d02fd5e730d8ebcc09dd241b924e8c50dcfca33512da3774a91836e46a25d
```

原始校验记录在 `/home/qianruo/kbuild/kernels/7.4-rc1/preparation/user-source-verification.json`，源树副本在 `.rock4d/source-verification.json`。

## 适配内容

内核树已应用 Kiln NPU 所需的电源域、IOMMU、设备树适配，以及 RK3576 JPEG 解码驱动和设备树节点。配置继承当前实时内核的功能，启用 PREEMPT_RT；JPEG 驱动作为模块构建。已部署版本未启用实验性 VEPU510 编码驱动；与现有 RKNN 用户库不兼容的 Rocket 路径也未启用。

后续的 [VEPU510 MPP 移植](../rockchip-vepu510-mpp/README.md) 已通过完整编译，正在准备独立后缀 `-vepu510` 的安装候选。该候选使用用户修改后的 1000 Hz 配置，并重编匹配的外置驱动；本目录的运行版配置仍为 250 Hz。候选尚待安装和硬件编码验证。

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
  /home/qianruo/kbuild/kernels/7.4-rc1/source \
  /home/qianruo/kbuild/kernels/7.4-rc1/records
```

流程依次构建内核 Debian 包、NPU、Wi-Fi 和蓝牙模块，任一步失败即停止，不启用忽略缺失符号的选项。`bindeb-pkg` 可能同时生成 `linux-libc-dev` 头文件包，脚本不会安装它，也不会替换宿主用户态库。

日志目录分别保存 `kernel.log`、`rknpu.log`、`wifi.log`、`bluetooth.log` 和各阶段时间、退出码。总状态见 `build-stage.txt`、`build-started.txt`、`build-finished.txt`、`build-exit-code.txt`；成功后 `artifacts.sha256` 记录 image、headers 包及四个外置模块的校验值。

完整构建于 2026 年 11 月 3 日启动。脚本在严格链接前清理外置模块预检产物，避免把缺少最终符号版本的临时 `.ko` 当成交付文件。完成状态以本轮日志和退出码为准，旧目录 `/home/qianruo/kbuild/kernels/7.4-rc1/preparation` 的日志仅作历史记录。

## 验证边界

本机已完成严格构建、安装与重启，实际运行 `7.4.0-rc1-npu-rt`、`SMP PREEMPT_RT`。开机检查确认四个外置驱动已加载、Wi-Fi 已联网，NPU 推理后的显示及编码计数持续增长，JPEG 使用硬解，26 个应用线程均为 FIFO。车辆跟随保持关闭。蓝牙仅验证模块加载，未进行配对测试。

## 本机安装

本轮完整构建于 2026 年 11 月 3 日 07:01:18 成功结束：3475 个内核树模块与四个外置模块均通过严格构建，image/headers 包通过 36 项独立检查。

```bash
sudo bash /home/qianruo/autonomous-driving-car/scripts/install_kernel74.sh
```

安装脚本先备份现有启动配置，临时保护 Armbian 自动改写 uInitrd 的钩子，安装匹配的 headers、image 和 DKMS 驱动，校验版本化 Image、设备树及 uInitrd，最后统一设置下次启动。Wi-Fi/蓝牙登记为 `aic8800-usb/5.0-kiln-linux74`，仅自动构建 7.4 内核。安装阶段保留原有源码、模块和固件，脚本本身不重启；本机之后执行的清理见下文。

安装记录保存在 `/home/qianruo/kbuild/kernels/7.4-rc1/records/install.log` 与 `installation-verification.json`，重启结果在 `boot-verification.json`。随后按用户要求，已清除 7.2.8、7.3 两个分支的旧启动内核、源码、包、模块、headers 和旧回退载荷；当前仅保留 7.4。原安装阶段的旧回退脚本已移除，不能再按旧备份路径执行回退。清理与目录迁移清单位于 `/home/qianruo/kbuild/records/maintenance-20261103/`。

## kbuild 目录

```text
kbuild/
  kernels/7.4-rc1/
    source/        当前内核源码及构建产物
    packages/      当前 image、headers、dbg、libc-dev 与打包元数据
    records/       构建、安装、启动验证与交付补丁包
    preparation/   适配来源、校验与干净驱动包
    provenance/    原始 Kiln 补丁
  models/          模型与量化资料
  media/           摄像头、编解码与时延资料
  tools/           浏览器、PDF 工具
  backups/         工程与模型备份
  records/         审查、历史记录与整理清单
```

构建脚本把新生成的软件包收集到 `packages/`。根目录保留八个必要的兼容软链接，供既有脚本、模型清单和虚拟环境使用；它们不代表重复副本。
