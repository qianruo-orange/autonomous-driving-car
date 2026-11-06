# 应用实时调度

应用的调度设置与生命周期统一声明在 `source/core/main.c` 的模块表中，包括线程策略、优先级和 CPU 分组。普通模块任务由 main 创建和回收；HTTP 管理自身线程，推理管理内部工作线程，创建属性均取自同一张表。各模块通过 register/unregister 管理看门狗。

| 线程角色 | 策略与优先级 | CPU 分组 |
| --- | --- | --- |
| main、TCP、HTTP reactor | FIFO 5 | LITTLE |
| HTTP worker、推理日报 | FIFO 1 | LITTLE |
| 图像渲染、录像 | FIFO 10 | BIG |
| NPU 推理 worker | FIFO 12 | BIG |
| **JPEG 软解 ×2** | **FIFO 15** | **BIG** |
| 相机采集 | **DEADLINE** runtime 2000 µs / deadline 34000 µs / period 34000 µs | ALL |
| CAN 收发 | FIFO 15 | LITTLE |
| 跟随控制 | FIFO 20 | LITTLE |
| 看门狗 | FIFO 30 | LITTLE |

相机采集线程使用 `SCHED_DEADLINE`，按 30 fps 的 33.3 ms 周期给出 34 ms 的周期与相对截止期，预算 2 ms（带宽约 5.9%）。内核的 `__check_sched_dl_overflow()` 要求 deadline 任务的亲和性**覆盖整个 root domain**，任何绑核都会让申请返回 `EPERM`；因此该行使用 `CPU_ALL` 而不是 `BIG`。作为代价，采集线程不再固定在大核上，实测最坏单次运行从绑核 FIFO 时的 0.48 ms 上升到 0.867 ms，仍为预算的 43%。申请被拒绝（内核不支持、准入控制拒绝、亲和性不符）时回退到表中的 FIFO 15，绝不阻断线程创建。2026-11-05 的 60 秒实测为 1971 次激活、单次最长 0.867 ms、零次超预算限流。`SCHED_DEADLINE` 无法通过 `pthread_attr_setschedpolicy()` 设置，实现使用线程创建后的 `sched_setattr()`。

JPEG 软解的 2 个线程走相反的取舍：优先绑核，放弃 deadline。本内核上两者互斥（实测：先绑大核再申请 DL 返回 `EPERM`；先申请 DL 再收窄亲和性返回 `EBUSY`；用 cgroup v2 cpuset 把任务收窄后再申请 DL 仍是 `EPERM`，因为 DL 准入要求亲和性覆盖整个 root domain），所以该行配 `CPU_BIG` 与 FIFO 15，与推理、渲染同用 4 个大核，靠高于推理的优先级保证解码先就绪。收益明确：绑定前单个软解线程只能解 25 fps，两个线程并行后跑满相机的 29 fps，整机多花约一个核的三分之一。线程名均为 `decode`，各持一个 `jpeg_decompress_struct`，解码结果仍写进各自的 8 槽 NV12 dma-buf 环供 RGA 读取。

CPU 分组按设备能力自动发现：先读 `cpu_capacity`，缺失时退到 `cpufreq/cpuinfo_max_freq`，同构或未知平台让两组都退回全部可用核。线程在进入任务函数前获得调度与亲和性属性；申请失败返回错误。`/api/follow` 分别报告控制线程的实际策略、优先级和内核 RT 状态。跟随控制在每次启动后默认关闭。

## 系统条件

部署目标需要 PREEMPT_RT 内核，以及允许服务申请实时调度的权限和运行时预算。2026-11-04 的开机检查确认本机运行 `7.4.0-rc1-npu-rt-vepu510`、PREEMPT_RT、1000 Hz；2026-11-05 复验时 18 个常驻应用线程中 17 个为 FIFO、相机采集为 `SCHED_DEADLINE`，JPEG 软解、推理、显示与录像计数持续增长。2026-11-06 拆分解码线程后复验：进程内 18 个常驻线程中 17 个 FIFO（含两个 `decode`，`Cpus_allowed_list` 均为 4-7）、相机采集仍为 `SCHED_DEADLINE`（`Cpus_allowed_list` 0-7）。验证记录位于本机 `/var/lib/rock4d-rt/boot-check.json`。

当前内核启用 `CONFIG_RT_GROUP_SCHED`。cgroup v2 的 CPU 控制器可能使服务子组缺少实时预算，导致申请 FIFO 或 deadline 返回 `EPERM`。`scripts/deploy.sh` 根据运行配置和启动参数判断，并在停止已有服务前处理和验证兼容设置：根 slice 使用 `DisableControllers=cpu`。该主机设置影响整个层级的 CPU 配额与权重；内存和进程数控制器继续工作。它不取消全局实时预算，卸载应用时也不会自动移除。

## 验证范围

2026-11-02 已验证线程创建属性、main 生命周期、部署兼容处理，以及模型重载和录像重建后的实际调度、优先级与 CPU 掩码。原始结果保存在本机 `/var/lib/rock4d-rt/main-scheduling-checks.json`。当时并发视频负载下，CPU1/FIFO 10 的 10000 次 1 ms 定时采样得到平均延迟 34.689 µs、P99 134.020 µs、最大 638.341 µs；这是历史短时测量，不代表当前构建负载，也不构成最坏时延保证。

CAN 实际总线、电机闭环与蓝牙配对不在上述验证范围内。内核构建、驱动补丁、安装与引导回退资料及内核报告 PDF 位于独立的 [rock-4d-npu-kernel](https://github.com/qianruo-orange/rock-4d-npu-kernel) 仓库；本应用仓库只维护用户态代码与部署逻辑。
