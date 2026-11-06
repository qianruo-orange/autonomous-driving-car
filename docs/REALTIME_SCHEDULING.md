# 应用实时调度

应用的调度设置与生命周期统一声明在 `source/core/main.c` 的模块表中，包括线程策略、优先级和 CPU 分组。普通模块任务由 main 创建和回收；HTTP 管理自身线程，推理管理内部工作线程，创建属性均取自同一张表。各模块通过 register/unregister 管理看门狗。

| 线程角色 | 策略与优先级 | CPU 分组 |
| --- | --- | --- |
| main、TCP、HTTP reactor | FIFO 5 | LITTLE |
| HTTP worker、推理日报 | FIFO 1 | LITTLE |
| 图像渲染、录像 | FIFO 10 | BIG |
| NPU 推理 worker | FIFO 12 | BIG |
| **JPEG 软解 ×2** | **FIFO 15** | **BIG** |
| **相机采集** | **FIFO 15** | **BIG** |
| CAN 收发 | FIFO 15 | LITTLE |
| 跟随控制 | FIFO 20 | LITTLE |
| 看门狗 | FIFO 30 | LITTLE |

相机采集线程使用 FIFO 15 绑在大核，与解码、推理同组：它绝大部分时间阻塞在 `DQBUF` 上，每 33.3 ms 唤醒一次，只做一次入环，单次运行不到 1 ms，因此固定优先级足够。更早的版本用过 `SCHED_DEADLINE`（runtime 2000 µs / deadline 34000 µs / period 34000 µs），代价是内核的 `__check_sched_dl_overflow()` 要求 deadline 任务的亲和性覆盖整个 root domain，采集线程只能在 8 个核之间漂移，实测最坏单次运行 0.867 ms，而绑核 FIFO 时为 0.48 ms。改用 FIFO 后全表只剩一种策略，线程创建走 `pthread_attr_setschedpolicy()`，不再需要创建后 `sched_setattr()` 的补丁路径。

JPEG 软解的 2 个线程同样绑 `CPU_BIG`，优先级 15 高于推理的 12，保证解码在推理取帧之前就绪。收益明确：单线程时只能解 25 fps，拆成两个线程后跑满相机的 29 fps，整机多花约一个核的三分之一。线程名均为 `decode`，各持一个 `jpeg_decompress_struct`，解码结果写进各自的 8 槽 NV12 dma-buf 环供 RGA 读取。

CPU 分组按设备能力自动发现：先读 `cpu_capacity`，缺失时退到 `cpufreq/cpuinfo_max_freq`，同构或未知平台让两组都退回全部可用核。线程在进入任务函数前获得调度与亲和性属性；申请失败返回错误。`/api/follow` 分别报告控制线程的实际策略、优先级和内核 RT 状态。跟随控制在每次启动后默认关闭。

## 系统条件

部署目标需要 PREEMPT_RT 内核，以及允许服务申请实时调度的权限和运行时预算。2026-11-04 的开机检查确认本机运行 `7.4.0-rc1-npu-rt-vepu510`、PREEMPT_RT、1000 Hz；2026-11-05 复验时 18 个常驻应用线程中 17 个为 FIFO、相机采集为 `SCHED_DEADLINE`，JPEG 软解、推理、显示与录像计数持续增长。2026-11-06 拆分解码线程后复验：进程内 18 个常驻线程全部为 FIFO（含两个 `decode`），`Cpus_allowed_list` 一律是 0-3 或 4-7，没有跨簇漂移的线程。验证记录位于本机 `/var/lib/rock4d-rt/boot-check.json`。

当前内核启用 `CONFIG_RT_GROUP_SCHED`。cgroup v2 的 CPU 控制器可能使服务子组缺少实时预算，导致申请 FIFO 返回 `EPERM`。`scripts/deploy.sh` 根据运行配置和启动参数判断，并在停止已有服务前处理和验证兼容设置：根 slice 使用 `DisableControllers=cpu`。该主机设置影响整个层级的 CPU 配额与权重；内存和进程数控制器继续工作。它不取消全局实时预算，卸载应用时也不会自动移除。

## 验证范围

2026-11-02 已验证线程创建属性、main 生命周期、部署兼容处理，以及模型重载和录像重建后的实际调度、优先级与 CPU 掩码。原始结果保存在本机 `/var/lib/rock4d-rt/main-scheduling-checks.json`。当时并发视频负载下，CPU1/FIFO 10 的 10000 次 1 ms 定时采样得到平均延迟 34.689 µs、P99 134.020 µs、最大 638.341 µs；这是历史短时测量，不代表当前构建负载，也不构成最坏时延保证。

CAN 实际总线、电机闭环与蓝牙配对不在上述验证范围内。内核构建、驱动补丁、安装与引导回退资料及内核报告 PDF 位于独立的 [rock-4d-npu-kernel](https://github.com/qianruo-orange/rock-4d-npu-kernel) 仓库；本应用仓库只维护用户态代码与部署逻辑。
