# 工程代码审查报告

审查日期：2026-11-01。基线：`83ff177`，另包含本轮 main 简化与控制线程心跳修正。下面的问题均针对当前工作区；除“本轮完成”中的内容外，其余问题的修复状态在对应条目中注明。

## 范围与结论

已通读项目维护的 C/C++ 实现、头文件、浏览器脚本、HTML/CSS、构建与部署脚本、现有测试，以及 CMake 和 systemd 配置。范围为 44 个实现文件、37 个已跟踪头文件、25 个前端文件、3 个脚本、1 个测试文件和 2 个构建/服务文件，共 112 个文件。RKNN 厂商头文件包含在接口阅读范围内；编译生成的 version.h、模型二进制、构建产物与运行日志不作为维护代码计入。

工程已有清晰的模块表、通信句柄和业务数据槽，视频也已形成共享采集、推理、标注和编码链路。当前优先事项是收紧失败路径与资源生命周期：发现的问题涉及可达的错误响应长度、CAN 恢复失效、畸形 DBC 解码、视频重启期间的引用保护，以及退出时的队列背压。继续减少行数应围绕这些边界进行，避免把错误处理一起删掉。

P1 表示可能导致崩溃、主要功能静默失效或退出丢失数据，应优先处理；P2 表示功能错误、并发风险或边界处理缺失。下列并发和硬件问题的控制流已经核对，但没有通过干扰现场设备来制造故障。

## 本轮完成

- main.c 从 371 行精简到 141 行。保留模块表，模块表只保留 name/init/dtor/task 四列并改为常量；用成功初始化的连续前缀代替每项 initialized 标记。
- 删除 app_thread_create、app_thread_start_detached 和空的 core/thread.h。main 直接将 m->task 交给 pthread_create，使用默认的可等待线程；推理模块直接创建自己的子线程。background_run、background_task_t 和额外堆分配已移除。
- 删除 app_thread_finished 和共享 threads_running 计数。main 保存成功创建的线程句柄，停止后在同一个 CLOCK_MONOTONIC 的 5 秒期限内逐个 join；创建失败不会登记无效句柄。HTTP 改 IP 的异步任务移入 http_api_config.c，由现有 HTTP worker 统计与等待；网络模块只保留同步网络操作。
- 删除 thread_wrapper。线程命名、绑核和看门狗 register/unregister 由各模块自己的入口承担；所有注册后的提前返回统一经过注销，注册失败和意外退出会请求失败停机。watchdog 监控线程继续免于自我注册。
- 删除 wait_threads_exit 和 shutdown_modules_safe，正常退出、启动失败共用 main 中的停止、等待和逆序析构段。工作线程未退出时仍保留 5 秒失败退出保护。
- 将 car_sport_ctrl 的心跳从 finish_motor_stop 的停机重试循环移到 follow_task 的常驻循环。此前正常运行的控制线程不喂狗，会被其 3 秒超时、3 次漏报策略判定失活。

## P1：优先修复

### 1. 录像列表超长时，负长度转换成 size_t 后进入 memcpy

位置：source/http/http_api_rec.c:111、source/http/http.c:265。

录像列表最多收集 1024 项，但 JSON 缓冲只有 65536 字节。http_json_append 溢出后返回 -1，serve_rec_list 仍把 off 转为 size_t 传给 http_ok_json。conn_append 使用 `c->wlen + len` 检查容量；当 len 为 SIZE_MAX 且已经追加响应头时，加法回绕，随后巨大的 memcpy 长度可以导致进程崩溃。

建议：构建失败必须返回明确错误，或者分页/流式输出；公共追加入口改用 `len > limit - current` 检查。两个边界都要修，不能只修列表调用者。

### 2. DBC 的负起始位未被拒绝

位置：source/can/dbc_parser.c:109、source/can/dbc_parser.c:195。

parse_signal 接受任意 int 起始位。Intel 路径只检查计算后的最大字节是否大于帧长度，没有检查 start_bit 非负；负值能进入 data[pos / 8] 与移位表达式。极端正值还可能使 start_bit + length - 1 发生有符号溢出。

建议：加载时验证起始位、宽度和报文 DLC；解码入口继续检查帧与信号边界，使用不会溢出的范围计算。无效定义应使上传失败，而不是保存到运行数据库。

### 3. 相机重启超时后仍解除 mmap，解码者可能继续读旧地址

位置：source/video/video_stream.c:514、source/video/frame_ring.c:409、source/yolo/rknn_yolo.c:1345。

YOLO 主任务持有 raw claim，并在锁外解码 mmap 数据。视频任务等待 claim 归零 1 秒后，无论是否超时都会调用 deinit_video_device 解除映射。代码注释假定超时后只剩读取 RGB 的推理 worker，但 claim 同样覆盖尚未完成的原始帧解码。调度停顿或慢解码时，这个假定不成立。

建议：仍有原始帧读者时保留映射；重启应延后、失败退出，或使用明确的代际与读者确认机制。迟到写槽时检查 seq 无法保护已经读入的裸指针。

### 4. CAN 重配置失败后的 epoll 恢复参数错误

位置：source/http/http_api_config.c:163。

配置前从接收 epoll 删除旧 fd，can_socket_configure 失败后却用 NULL event 执行 EPOLL_CTL_ADD。该恢复调用无法成功，旧 socket 虽然保留，接收线程却再也得不到它的事件。配置字段也已经提前写入，失败时没有恢复旧值。

建议：复用统一的接收注册操作并检查返回值；把解析、硬件应用、socket/filter/epoll 准备和运行态提交做成一次事务。

### 5. 工作线程提前返回会从看门狗消失（本轮已修复）

位置：各模块 task 入口；例如 source/http/http.c 的 http_server_task。

模块入口无条件在 task 返回后注销看门狗。HTTP socket 创建、绑定等路径可能直接返回，此时 app.running 仍为真；main 无法识别 HTTP 已经永久停止，看门狗也不再监控它。服务可能显示正常运行，实际没有 Web 服务。

修复：模块入口现在检测 task 返回时的运行标志；应用仍运行时记录错误并请求失败退出。看门狗注册失败也走同一失败处理，正常退出归还一次计数。

### 6. 日报写入失败时，停机准备晚于主任务退出等待

位置：source/yolo/rknn_yolo.c:400、source/yolo/rknn_yolo.c:1004、source/yolo/yolo_events.c:517、source/core/main.c:119。

任务队列满时生产者等待 q_not_full；日报存储持续失败时 worker 又可能等待日报队列空间。main 先等待模块主任务返回，yolo_events_prepare_stop 却要到之后的 YOLO 析构才执行。生产者无法被停机释放，最终进入 5 秒失败退出，录像和已接受的日报可能来不及收尾。

建议：先发出不释放资源的停止通知并唤醒阻塞队列，再等待线程，最后析构。可优先在 YOLO 模块自身处理退出通知，保持 main 的层级简单。

### 7. V4L2 编码缓冲返回数量没有上界检查

位置：source/video/h264_encoder.c:389。

buf_in/buf_out 为 ENC_BUFS=3 的固定数组；VIDIOC_REQBUFS 返回后只检查 count >= 2，映射循环却以驱动返回的 req.count 为上界。如果驱动要求或返回更多缓冲，就会写出数组边界。当前 FFmpeg 路径不会覆盖这个 V4L2 回退分支。

建议：按实际数量分配并保存数量，或者明确拒绝超过固定容量的返回值；映射、排队、销毁统一使用同一个已验证数量。

## P2：功能与边界问题

| 编号 | 位置 | 已确认的问题与触发条件 | 建议 |
| --- | --- | --- | --- |
| 8 | source/http/http_api_yolo.c:123 | 把包含 `?` 的字符串直接交给查找 `type=` 的表单解析器；正常的 `?type=model` / `?type=names` 匹配失败，上传返回 bad type。 | 传入 q+1；让查询参数与表单体复用经过明确切分的解析入口。 |
| 9 | source/can/dbc_parser.c:71 | Motorola 位提取用 start-i 和反转的字节内位号，与 DBC 的跨字节位编号不符。例如 start=7、len=1 应取 bit7，当前取 bit0。 | 使用明确的跨字节步进规则，并覆盖单字节、跨字节与符号扩展向量。 |
| 10 | source/can/can_socket.c:898；source/http/http_api_can.c:117 | 从 ifindex=1 连续查询，遇到第一个缺号就终止。接口删除后 ifindex 常有空洞，后面的 CAN 会被漏掉。 | 用 netlink link cache/dump 遍历实际接口，状态 API 共用枚举结果。 |
| 11 | source/can/can_socket.c:484 | 已有接口的 shell 配置以最后一条命令状态为结果，type can 配置失败而 up 成功时仍报告成功；关闭 FD 没有显式 fd off，restart_ms 也未应用。 | 分别检查操作结果并读取真实参数；已有和新建接口共用完整参数应用规则。 |
| 12 | source/video/h264_encoder.c:60；source/video/rec_mp4.c:356 | Annex-B 遍历器可能对只有尾部起始码的输入回调 nlen=0，SPS/PPS 回调先读取 start[0] 再判断长度。 | 公共遍历器跳过空 NAL，消费者仍先验证长度。 |
| 13 | source/http/http_api_config.c:243；source/yolo/yolo_draw.cpp:119 | API 接受奇数宽高；NV12/I420 转换要求偶数尺寸，cvtColor 调用位于异常捕获范围外。若相机实际协商出奇数尺寸，异常可能终止进程。 | 同时验证配置与驱动协商后的尺寸；在 C++ 边界内处理转换异常。 |
| 14 | source/yolo/rknn_yolo.c:1004、:1053、:1188、:1303 | 热重载只停止 worker/render，不等待主生产者。主任务可能正在解码/投递，重载同时重置 last_seq 和队列，存在数据竞争及旧任务跨代投递。 | 给生产者与池切换增加代际/暂停确认；停止后再清队列与重建。 |
| 15 | source/video/rec_mp4.c:142 | mvhd 使用 1000 的时间基，tkhd 却直接写入 delta*N 的 90000 时间基值；mvhd 的首末时间差还少算最后一帧。 | 区分电影时间基与媒体时间基，统一计算总样本时长。 |
| 16 | source/video/rec_mp4.c:423 | finalize 忽略 ftell/fseek、moov 写入及 fclose 错误，直接将 rc 设为成功。磁盘满或 I/O 失败时可能报告录像保存成功。 | 写入函数返回状态，检查 ferror/flush/close，并向录制状态传播失败。 |
| 17 | source/http/http_util.c:167；source/http/http.c:525 | Content-Length 只匹配全大写首字母或全小写写法，并且没有限定头字段边界；认证头也在整个请求中搜索，可能把正文或其他字段中的文本当成头。 | 头部结束后逐行解析，字段名大小写无关，严格校验长度、重复字段和数字结尾。 |
| 18 | source/http/http_api_nettest.c:104、:189、:259 | target 被拼到单引号 shell 参数中，但校验未拒绝单引号和重定向字符，可改变命令参数或重定向输出。接口本身要求管理员认证，不能据此称为匿名代码执行。 | 使用 posix_spawn/exec 的参数数组调用 ping/curl，避免构造 shell 命令。 |
| 19 | source/core/config.c:215；source/http/http_api_config.c:408 | config_save 为 void，仅记录保存失败；HTTP 仍返回 saved。运行时配置与磁盘可能不一致，重启后恢复旧值。 | 让保存返回状态；HTTP 成功应以应用与持久化都成功为前提。 |
| 20 | source/tcp/net_ip.c:26、:67 | 掩码只检查非空，非法字符串默认 /24，非连续掩码只统计前导 1。旧地址会先被 flush，再应用未经正确验证的网络范围。 | 在任何接口修改前验证完整地址、连续掩码与网关，并明确拒绝错误输入。 |
| 21 | source/watchdog/watchdog.c:70、:151 | 超时和 systemd 通知调度使用 time(NULL)。系统校时可能提前触发超时或延迟通知。 | 存活判断统一使用 CLOCK_MONOTONIC，墙上时间只用于日志。 |
| 22 | source/http/http.c:307 | conn_drain 在每次续填后递归调用自身，每一层重新计算 1MB 预算；快连接的大文件可能持续占用 reactor，Debug 构建还可能堆积调用栈。 | 改为单层循环并在整次事件内共享字节预算。 |
| 23 | source/http/http.c:925 | 上传使用可预测的 /tmp/yolo_upload_fd.tmp 和 fopen wb，且完整落盘后才认证。直接运行时有临时文件跟随链接风险；服务的 PrivateTmp 可缩小该风险，但未认证上传仍会消耗临时盘空间。 | 使用 mkstemp/O_EXCL；读完请求头后先认证并验证精确 body 上限。 |
| 24 | html/js/car_sport_ctrl.js:195；html/js/logs.js:274；html/js/detections.js:135；html/js/monitor.js:743 | 部分模块仍用 setInterval 或无超时 fetch。慢请求可重叠或永久卡住刷新；日志/结果页 pagehide 清除定时器后，浏览器历史缓存恢复没有重新启动。 | 将这些轮询迁入现有 App.poll/App.json，统一超时、页面隐藏与恢复行为。 |

## 复用与结构调整顺序

1. **先处理返回值与长度。** JSON 追加、文件写入、配置保存必须传递失败状态；旧 JSON_ADD 宏直接 return 的行为也会产生无响应，应逐步统一成可检查的构建结果。
2. **统一 CAN 的配置事务。** SocketCAN 配置和 HTTP 配置现在分别修改字段、socket、过滤器与 epoll；应由 CAN 模块暴露一个应用配置入口，HTTP 只做解析和响应。
3. **明确停止通知与资源销毁的边界。** 保留 main 中的一段启动、一段等待、一段逆序析构；队列唤醒、子线程 join 和推理代际由对应模块负责。
4. **统一媒体边界处理。** Annex-B 扫描、合法尺寸、SPS/PPS 校验、时间换算各保留一个实现，让直播与录像使用相同规则。
5. **收拢剩余浏览器轮询。** 已有 App.json/App.poll 足够使用，无需再增加另一套前端组件框架。保留业务模块自己的 DOM 与状态。
6. **避免继续拆分只有转发作用的函数。** 模块表与必要线程入口保留清晰职责；临时任务直接执行业务入口，取消堆分配包装。不要为减少 main 行数重新增加 init/start/dispatch/run 多层调用。

## 验证记录与限制

- 通过 `bash scripts/build.sh -R` 完成最新版 Release 编译与链接，目标为 bin/autonomous-driving-car。
- `git diff --check` 通过。
- 原 tests/http_input_test.c 编译运行通过；该测试代码随后按要求删除。
- 使用实际模块对象验证 8 个入口的看门狗注册失败、6 个入口的提前失败以及录像线程正常停机，核对注册/注销次数与 fatal/running 标志；屏蔽了设备调用。
- 临时 main 启动验证通过：正常启动、早期/晚期初始化失败、首个/后续线程创建失败，均验证工作线程退出及逆序清理；另注入 join 超时，确认失败退出且不执行析构。
- 临时 HTTP IP 任务验证通过：参数独立复制、停机后跳过应用、worker 创建失败；网络操作使用替身，未改变设备网络。
- 临时 HTTP 解析验证确认：`?type=model` 查找返回 0，去掉问号后返回 5；规范 Content-Length 返回 3，混合大小写字段返回 -1。
- 临时验证源码与二进制曾位于 /tmp，随后已按要求删除；工程未保留测试代码。
- 本轮未通过重启服务、改网络、发送电机命令、填满磁盘或替换模型制造故障。编译成功与静态审查不代表这些硬件和故障场景已完成运行验证。

本报告对应本轮工作区。后续修复应更新条目状态并进行相应验证，不能把报告中的待修复项视为已经完成。
