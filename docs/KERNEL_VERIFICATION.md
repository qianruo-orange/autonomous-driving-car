# 内核与实时调度验证

验证时间：2026-11-02 上午，Asia/Shanghai。设备为 Radxa ROCK 4D / RK3576。

## 内核

实际运行 `7.3.0-npu-rt`，版本包含 `SMP PREEMPT_RT`；运行内核的 `/proc/config.gz` 确认 `CONFIG_PREEMPT_RT=y`。本机没有旧的 `/sys/kernel/realtime`，应用现在也通过运行内核版本识别 RT，避免将缺少旧接口误报为非 RT。

内核包含 Kiln NPU 补丁。3478 个内核模块已构建，NPU、Wi-Fi 与蓝牙相关四个 DKMS 模块已安装，版本标记与当前 RT 内核一致。`rock4d-rt-boot-check.service` 已完成开机检查，结果保存在 `/var/lib/rock4d-rt/boot-check.json`。旧内核与引导备份保留在设备上。

## 调度归属

`source/core/main.c` 的模块表同时声明初始化、销毁、任务、`policy`、`priority` 和 `cpus`；不再维护独立调度表或全局固定策略，创建属性直接读取对应行。当前线程行均为 `SCHED_FIFO`。线程从入口执行前就具备指定优先级，创建失败返回错误。各模块不再修改调度策略或自行绑核；HTTP 和推理模块继续管理内部线程的创建时机与回收。没有恢复入口包装层。

| 线程 | FIFO 优先级 | CPU 分组 |
| --- | ---: | --- |
| main、TCP、HTTP reactor | 5 | LITTLE |
| HTTP worker、推理日报 | 1 | LITTLE |
| 图像解码、渲染、录像与编码器辅助线程 | 10 | BIG |
| NPU 推理 worker | 12 | BIG |
| 相机采集 | 15 | BIG |
| CAN 收发 | 15 | LITTLE |
| 跟随控制 | 20 | LITTLE |
| 看门狗 | 30 | LITTLE |

`cpus` 通过 CPU 能力或最高频率自动发现分组，先保存完整初始允许集合，再绑定 main；普通工作线程在创建前通过属性绑定，不增加入口包装层。当前设备 BIG 为 CPU4–7，LITTLE 为 CPU0–3；源码不硬编码核心编号。非线程行的 `policy=-1` 防止误创建；有效策略允许优先级为 0 的 `SCHED_OTHER`，`CPU_INHERIT` 可保留父线程掩码。

JPEG 解码与编码在解码、渲染线程内同步执行。OpenCV 初始化时设为单线程，避免库线程池绕开调用线程的调度属性；libx264 辅助线程继承录像线程的 FIFO 10。本机实测录像线程加编码器辅助线程共 9 个，逐个查询系统调度属性确认。

## 重启兼容问题

复现了开机成功、服务重启却申请 FIFO 返回 `EPERM` 的问题。当前内核启用了 `CONFIG_RT_GROUP_SCHED`；cgroup v2 的根 CPU 控制器启用后，新子组的实时预算为零，而 v2 没有对应实时预算接口。相同 root 权限的隔离服务同样失败，增加权限不能解决这个分组限制。对应内核分支位于 `kernel/sched/syscalls.c` 的零实时预算检查。

本机通过 `/etc/systemd/system/-.slice.d/90-autonomous-driving-car-rt.conf` 设置 `[Slice] DisableControllers=cpu`。部署脚本根据运行内核配置与 `rt_group_sched=0/1` 启动参数判断是否需要此设置，并在停止已有服务前应用和验证；失败时保留已有服务。该主机设置影响整个 cgroup 层级的 CPU 配额和权重，卸载程序时保留。本机没有已有的自定义 CPU 配额或权重；内存、进程数控制器继续工作，全局实时预算仍为 `1000000/1000000`，没有修改为无限制。

设置后连续三次隔离服务均成功设置 FIFO 5、继承 FIFO 5、切换 FIFO 30。行为与 [Linux cgroup v2 文档](https://docs.kernel.org/admin-guide/cgroup-v2.html) 和 [systemd 控制器说明](https://www.freedesktop.org/software/systemd/man/latest/systemd.resource-control.html) 一致。

## 验证结果

- 项目脚本 Release 构建、`git diff --check`、部署脚本语法检查通过。
- 直接链接实际 main 创建函数的 50 项隔离检查通过：14 个角色的可连接/分离属性、无效参数和无任务模块、8 个失败阶段；确认策略、优先级、CPU 掩码、参数传递、失败释放及无回退创建。另有 6 项仅修改临时表行的检查，确认 RR、普通策略及 CPU 掩码继承生效。
- 实际 main 生命周期的 4 项隔离检查通过：仅创建 9 个模块任务，内部线程配置行不自动创建线程，逆序回收正确；main 获取 CPU 分组、绑核或调度失败时不创建工作线程。亲和性接口另验证有效分组、无效参数及初始化失败，未修改测试进程的 CPU 掩码。
- 部署兼容检查的 15 项隔离验证通过，涵盖内核默认值、启动参数、配置回退、失败路径，以及 `-i`、`-r` 失败时不停止已有服务。
- 实机逐一检查 26 个常驻线程及 HTTP 工作线程；本轮并发视频期间共观察到 30 个线程，全部策略、角色优先级与 CPU 分组正确。
- 实机检查通过，服务 PID 保持不变、重启计数为 0，配置恢复原值。已验证登录主页、13 个公开状态接口、原始/标注 MJPEG 和 fMP4；标签及原模型上传、置信度变更及恢复、录像分段、停止/启动后，重新逐个确认新建推理、报告和编码线程的策略、优先级及 CPU 掩码。
- 三路视频负载期间 5 秒采样：采集序号 +125、显示序号 +125、编码帧 +125；该采样内驱动丢帧与采集丢帧增量均为 0。模型重载与录像重建包含计划内暂停，这些数字不表示全程没有丢帧。

在实时应用及并发视频请求期间另运行原生定时采样：CPU1、FIFO 10、锁定内存、10000 次 1ms CLOCK_MONOTONIC 绝对等待。延迟为平均 34.689µs、P99 134.020µs、最大 638.341µs，超过或等于 1ms 的次数为 0。原始结果位于 `/var/lib/rock4d-rt/main-scheduling-latency.json`。这是实际负载下的短时采样，不构成最坏时延保证。

验证脚本和创建属性测试仅位于 `/tmp`，未加入项目测试目录。实机详细结果保存在设备的 `/var/lib/rock4d-rt/main-scheduling-checks.json`（临时原件 `/tmp/car-main-scheduling-results.json`），其中包含各阶段线程 ID、策略及优先级。

## 边界

本轮没有配置 CAN 与电机，也没有启用车辆跟随驱动；实际总线、电机闭环未验证。当前验证使用软件编解码，未验证硬件编解码。蓝牙模块加载不等于已完成配对或传输测试。高负载下板卡曾出现热限频；既有 UFS、设备链接等启动警告及审查报告中的其他问题未在本轮解决。
