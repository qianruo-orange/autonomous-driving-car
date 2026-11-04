# 应用实时调度

应用的调度设置与生命周期统一声明在 `source/core/main.c` 的模块表中，包括线程策略、优先级和 CPU 分组。普通模块任务由 main 创建和回收；HTTP 管理自身线程，推理管理内部工作线程，创建属性均取自同一张表。各模块通过 register/unregister 管理看门狗。

| 线程角色 | FIFO 优先级 | CPU 分组 |
| --- | ---: | --- |
| main、TCP、HTTP reactor | 5 | LITTLE |
| HTTP worker、推理日报 | 1 | LITTLE |
| 图像解码、渲染、录像与编码器辅助线程 | 10 | BIG |
| NPU 推理 worker | 12 | BIG |
| 相机采集 | 15 | BIG |
| CAN 收发 | 15 | LITTLE |
| 跟随控制 | 20 | LITTLE |
| 看门狗 | 30 | LITTLE |

CPU 分组按设备能力自动发现。线程在进入任务函数前获得调度与亲和性属性；申请失败返回错误。`/api/follow` 分别报告控制线程的实际策略、优先级和内核 RT 状态。跟随控制在每次启动后默认关闭。

## 系统条件

部署目标需要 PREEMPT_RT 内核，以及允许服务申请实时调度的权限和运行时预算。2026-11-04 的开机检查确认本机运行 `7.4.0-rc1-npu-rt-vepu510`、PREEMPT_RT、1000 Hz；26 个常驻应用线程均为 FIFO，JPEG 硬解、推理、显示与录像计数持续增长。验证记录位于本机 `/var/lib/rock4d-rt/boot-check.json`。

当前内核启用 `CONFIG_RT_GROUP_SCHED`。cgroup v2 的 CPU 控制器可能使服务子组缺少实时预算，导致申请 FIFO 返回 `EPERM`。`scripts/deploy.sh` 根据运行配置和启动参数判断，并在停止已有服务前处理和验证兼容设置：根 slice 使用 `DisableControllers=cpu`。该主机设置影响整个层级的 CPU 配额与权重；内存和进程数控制器继续工作。它不取消全局实时预算，卸载应用时也不会自动移除。

## 验证范围

2026-11-02 已验证线程创建属性、main 生命周期、部署兼容处理，以及模型重载和录像重建后的实际调度、优先级与 CPU 掩码。原始结果保存在本机 `/var/lib/rock4d-rt/main-scheduling-checks.json`。当时并发视频负载下，CPU1/FIFO 10 的 10000 次 1 ms 定时采样得到平均延迟 34.689 µs、P99 134.020 µs、最大 638.341 µs；这是历史短时测量，不代表当前构建负载，也不构成最坏时延保证。

CAN 实际总线、电机闭环与蓝牙配对不在上述验证范围内。内核构建、驱动补丁、安装与引导回退资料及内核报告 PDF 位于独立的 [rock-4d-npu-kernel](https://github.com/qianruo-orange/rock-4d-npu-kernel) 仓库；本应用仓库只维护用户态代码与部署逻辑。
