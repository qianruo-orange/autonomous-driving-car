// SPDX-License-Identifier: GPL-3.0-or-later
/* i18n.js - bilingual UI (zh / en) with a language toggle.
   The Chinese text is used as the lookup key: in zh mode t() returns it unchanged,
   in en mode t() returns the English string from the dictionary (falling back to
   the Chinese text when a translation is missing). */
(function () {
  var EN = {
    'VOFA 上位机': 'VOFA Telemetry',
    '通信状态': 'Connection',
    '停用 VOFA': 'Off',
    '启用 VOFA': 'On',
    '正在读取 VOFA 状态…': 'Loading VOFA status...',
    '正在保存 VOFA 设置…': 'Saving VOFA settings...',
    '发送频率 (Hz)': 'Send rate (Hz)',
    '数据协议': 'Protocol',
    'VOFA 连接地址': 'VOFA connection address',
    'VOFA 已关闭': 'VOFA is off',
    'VOFA 已连接': 'VOFA connected',
    'VOFA 等待连接': 'VOFA waiting for a client',
    'VOFA 未启动，请检查端口': 'VOFA is not listening; check the port',
    '客户端：{count} / {max}': 'Clients: {count} / {max}',
    '无法读取 VOFA 状态': 'Unable to read VOFA status',
    'VOFA 设置已保存': 'VOFA settings saved',
    '无法保存 VOFA 设置，请检查端口是否被占用': 'Unable to save VOFA settings; check whether the port is in use',
    '积分限幅': 'Integral limit',
    '限制积分项输出，范围 0–1；0 禁用积分': 'I-term output limit 0–1; 0 disables integration',
    '自动调速': 'Automatic speed control',
    '手动调速': 'Manual speed control',
    '转向参数': 'Steering parameters',
    '最大转向修正量 (m/s)': 'Maximum steering correction (m/s)',
    '比例系数': 'Proportional coefficient',
    '积分系数': 'Integral coefficient',
    '微分系数': 'Derivative coefficient',
    '跟踪设置': 'Tracking settings',
    '速度策略': 'Speed Strategy',
    '自动调速 PD': 'Automatic speed PD',
    '转向 PD': 'Steering PD',
    '轮速 PI': 'Wheel speed PI',
    '轮速 Kp': 'Wheel speed Kp',
    '轮速 Ki': 'Wheel speed Ki',
    '轮速 Kd': 'Wheel speed Kd',
    '实时调度错误': 'Real-time scheduling error',
    '按目标大小自动调速': 'Automatic from target size',
    '目标大小误差': 'Target size error',
    '智能跟随':'Intelligent Following',
    '电机 CAN 协议接口预留，未接入时仅预览；每次启动默认停止。':'Motor CAN protocol adapter reserved. Preview only until connected; stopped on every startup.',
    '控制状态':'Controller state','速度模式':'Speed mode','跟踪目标':'Tracking target','目标类别 ID':'Target class ID',
    '最大轮速 (m/s)':'Maximum wheel speed (m/s)','手动目标速度 (m/s)':'Manual target speed (m/s)',
    '最大转向角速度 (rad/s)':'Maximum yaw rate (rad/s)','左右轮间距 (m)':'Wheel track (m)',
    '中心误差死区':'Center error deadband','过期停止时间 (ms)':'Stop timeout (ms)',
    '自动速度 Kp':'Automatic speed Kp','自动速度 Ki':'Automatic speed Ki','自动速度 Kd':'Automatic speed Kd',
    '转向 Kp':'Steering Kp','转向 Ki':'Steering Ki','转向 Kd':'Steering Kd',
    '左轮速度 Kp':'Left wheel speed Kp','左轮速度 Ki':'Left wheel speed Ki','左轮速度 Kd':'Left wheel speed Kd',
    '右轮速度 Kp':'Right wheel speed Kp','右轮速度 Ki':'Right wheel speed Ki','右轮速度 Kd':'Right wheel speed Kd',
    '停止':'Stopped','启用跟随':'Enable following','按中心误差自动调速':'Automatic from center error','手动设定速度':'Manual speed',
    '已保存的目标':'Saved target','识别数据过期':'Detection data stale','目标丢失':'Target lost','等待下一识别帧':'Waiting for next detection',
    '未接入电机协议':'Motor protocol not connected','需要实时内核':'Real-time kernel required','实时调度不可用':'Real-time scheduling unavailable',
    '轮速反馈过期':'Wheel feedback stale','正在控制':'Controlling','参数已更新':'Settings updated',
    '电机发送失败':'Motor send failed','电机停止失败':'Motor stop failed','中心误差':'Center error',
    '左右轮目标速度':'Left/right target speed','左右轮控制输出':'Left/right control output','识别帧':'Detection frame',
    '控制状态读取失败':'Cannot read controller state','控制参数保存失败':'Cannot save controller settings',
    '启用 CAN': 'Enable CAN',
    '停用 CAN': 'Disable CAN',
    'CAN 已启用': 'CAN enabled',
    'CAN 已停用': 'CAN disabled',
    'CAN 开关保存失败，请重试': 'Unable to save the CAN setting. Please retry.',
    '配置加载失败，请点击查询重试': 'Unable to load settings. Select Refresh to retry.',
    '预览': 'Preview',
    '文件预览': 'File preview',
    '文件操作': 'File actions',
    '关闭': 'Close',
    '打包全部文件': 'Download all files',
    '删除全部文件': 'Delete all files',
    '正在打包全部文件…': 'Preparing all files...',
    '已开始打包下载，请在浏览器下载列表查看进度': 'Archive download started. Check your browser downloads for progress.',
    '暂无可下载文件': 'No saved files to download',
    '确定删除全部日志、录像和结果？此操作不可恢复，正在录制的文件会保留。': 'Delete all logs, saved recordings and results? This cannot be undone. The active recording will be kept.',
    '已删除 {count} 个文件，正在录制的文件已保留': 'Deleted {count} files. The active recording was kept.',
    '删除全部文件失败，请重试': 'Unable to delete all files. Please retry.',
    '正在加载预览…': 'Loading preview...',
    '可播放已保存的录像': 'Play the saved recording below',
    '预览已截断，文件总大小 {size}': 'Preview truncated. Total file size: {size}',
    '文件大小 {size}': 'File size: {size}',
    '预览已取消': 'Preview cancelled',
    '无法预览此文件，请重试': 'Unable to preview this file. Please retry.',
    '请先下载以保存当前录像片段': 'Use Download to save the active recording segment first',
    '无法播放此录像，请检查文件': 'Unable to play this recording. Check the file.',
    '相机': 'Camera',
    '相机调焦': 'Camera focus',
    '全屏': 'Fullscreen',
    '自动对焦': 'Autofocus',
    '手动焦距': 'Manual lens position',
    '应用焦距': 'Apply focus',
    '此相机不支持调焦': 'This camera does not support focus controls',
    '相机调焦不可用': 'Camera focus is unavailable',
    '正在应用焦距…': 'Applying focus...',
    '焦距已更新': 'Focus updated',
    '调焦失败，请重试': 'Focus failed. Please try again.',
    '相机配置': 'Camera settings',
    '分辨率': 'Resolution',
    '帧率': 'Frame rate',
    '相机始终使用连续自动对焦。': 'The camera always uses continuous autofocus.',
    '镜头位置为设备原始值，不是毫米焦距。': 'Lens position is a raw device value, not focal length in millimeters.',
    '镜头位置：{value}（设备原始值）': 'Lens position: {value} (raw device value)',
    '识别结果': 'Detections',
    '结果': 'Results',
    '清空结果': 'Clear results',
    '结果正在更新，请稍后重试': 'Results are updating; please retry shortly',
    '结果下载失败，请重试': 'Unable to download the result. Please retry',
    '暂无结果文件': 'No result files',
    '暂无可打包的结果': 'No results to archive',
    '确定清空全部结果？此操作不可恢复，后续检测将继续创建当天文件。': 'Clear all results? This cannot be undone. New detections will create today\'s report again.',
    '已清空 {count} 个结果文件': 'Cleared {count} result files',
    '清空结果失败，请重试': 'Unable to clear results. Please try again.',
    '每日检测汇总': 'Daily detection reports',
    '识别照片': 'Detection photos',
    '暂无检测汇总': 'No daily detection reports',
    '删除日报 {file}？后续检测将继续创建当天文件。': 'Delete report {file}? New detections will recreate today\'s file.',
    '显示最近 200 条已保存的识别结果。': 'Showing the latest 200 completed detections.',
    '检测到目标时自动保存，每秒最多一张。照片与 JSON 检测数据使用相同文件名。': 'Detections are saved automatically, up to one snapshot per second. The photo and JSON data share the same filename.',
    '捕捉时间': 'Capture time',
    '目标数': 'Objects',
    '镜头位置': 'Lens position',
    '未知': 'Unknown',
    '暂无识别结果': 'No saved detections',
    '下载照片': 'Photo',
    '下载检测数据': 'Detection JSON',
    '识别结果下载失败，请重试': 'Unable to download the detection. Please retry',
    '视频采集': 'Video capture',
    '相机设备': 'Camera device',
    '视频编码': 'Video encoder',
    '视频解码': 'Video decoder',
    '软件解码': 'Software decoding',
    '硬件解码（不可用）': 'Hardware decoding (unavailable)',
    '硬件编码（不可用）': 'Hardware encoding (unavailable)',
    '分辨率与帧率': 'Resolution / frame rate',
    '软件编码': 'Software encoding',
    '硬件编码': 'Hardware encoding',
    '仅显示相机与编码器共同支持的分辨率和帧率。': 'Only resolutions and frame rates supported by both the camera and encoder are shown.',
    '保存采集参数后，视频和录像会重新启动。': 'Saving capture settings restarts video and recording.',
    '相机与编码器没有共同支持的采集模式': 'The camera and encoder have no shared capture modes',
    '无法读取相机能力，请重试': 'Unable to read camera capabilities. Please retry',
    '正在读取相机配置…': 'Loading camera settings...',
    '当前系统没有可用的硬件编码器。': 'No hardware encoder is available on this system.',
    '无法确认硬件编码器状态。': 'Unable to confirm hardware encoder availability.',
    '无法读取相机配置，请重试': 'Unable to load camera settings. Please retry',
    '正在保存相机配置…': 'Saving camera settings...',
    /* ---- brand / navigation / common ---- */
    '自动驾驶小车': 'Autonomous Driving Car',
    '系统配置': 'System Configuration',
    '仪表盘': 'Dashboard',
    '主界面': 'Home',
    '识别结果正在更新，请稍后重试': 'Detection reports are updating; please retry shortly',
    'DBC解析': 'DBC Decoder',
    'DBC 信号解析': 'DBC Signal Decoder',
    '行驶日志': 'Driving Logs',
    '主题': 'Theme',
    '切换主题': 'Toggle theme',
    '切换语言': 'Switch language',
    '重启': 'Reboot',
    '关机': 'Shutdown',
    '退出登录': 'Sign out',
    '保存': 'Save',
    '查询': 'Refresh',
    '上传': 'Upload',
    '导出配置': 'Export config',
    '全部': 'All',
    '操作': 'Action',
    '日期': 'Date',
    '文件名': 'File',
    '大小': 'Size',
    '修改时间': 'Modified',
    '接口': 'Interface',
    '报文': 'Message',
    '信号值': 'Signal values',
    '等待数据…': 'Waiting for data...',
    '取消': 'Cancel',
    '确定': 'OK',
    '登录': 'Sign in',
    '密码': 'Password',
    '用户名': 'Username',

    /* ---- dashboard (monitor.js) ---- */
    '内存': 'Memory',
    '磁盘': 'Disk',
    '占用：': 'Used: ',
    '总量：': 'Total: ',
    '网络速度': 'Network',
    '有线': 'Wired',
    '无线': 'Wireless',
    '已连接': 'Connected',
    '未连接': 'Disconnected',
    '摄像头 {camera} FPS · 处理 {processing} ms · 接收 {received} FPS · 缓冲 {buffer} ms · 总延迟约 {delay} ms · 当前丢帧 {dropped}/s': 'Camera {camera} FPS · Processing {processing} ms · Received {received} FPS · Buffer {buffer} ms · Total delay ~{delay} ms · Dropped {dropped}/s',
    '摄像头 {camera} FPS · 处理 {processing} ms · 接收 {received} FPS · 缓冲 {buffer} ms · 推流延迟约 {delay} ms · 当前丢帧 {dropped}/s': 'Camera {camera} FPS · Processing {processing} ms · Received {received} FPS · Buffer {buffer} ms · Stream delay ~{delay} ms · Dropped {dropped}/s',
    '接收 {received} FPS · 缓冲 {buffer} ms · 推流延迟约 {delay} ms · 丢帧 {dropped}': 'Received {received} FPS · Buffer {buffer} ms · Stream delay ~{delay} ms · Dropped {dropped}',
    '星期日': 'Sunday', '星期一': 'Monday', '星期二': 'Tuesday',
    '星期三': 'Wednesday', '星期四': 'Thursday', '星期五': 'Friday', '星期六': 'Saturday',
    '确定要重启设备吗？': 'Are you sure you want to reboot the device?',
    '确定要关闭设备吗？': 'Are you sure you want to shut down the device?',

    /* ---- YOLO detection card ---- */
    'YOLO 检测': 'YOLO Detection',
    '推理参数': 'Inference',
    '推理线程数': 'Threads',
    '置信度': 'Confidence',
    '推理间隔': 'Interval',
    '模型文件': 'Model files',
    '上传模型': 'Upload model',
    '上传标签': 'Upload labels',

    /* ---- CAN card ---- */
    '波特率': 'Bitrate',
    'DBC 文件': 'DBC file',
    'FD波特率': 'FD bitrate',
    'CAN-ID 过滤器': 'CAN-ID filters',
    '+ 添加': '+ Add',
    '接口已关闭，开启后可配置': 'Interface is off; turn it on to configure',
    '系统未检测到 CAN 设备': 'No CAN device detected',
    '无可用 CAN 接口': 'No CAN interface available',
    '(无 CAN-FD)': '(no CAN-FD)',

    /* ---- network card ---- */
    '网络设置': 'Network',
    '绑定网卡': 'Bind interface',
    '所有网卡': 'All interfaces',
    'TCP端口': 'TCP port',
    '最大客户端数': 'Max clients',
    'IP 模式': 'IP mode',
    '关闭（不管理）': 'Off (unmanaged)',
    '静态': 'Static',
    'IP 地址': 'IP address',
    '子网掩码': 'Subnet mask',
    '网关': 'Gateway',
    '当前未读取': 'Not read yet',

    /* ---- video card ---- */
    '视频': 'Video',
    '设备': 'Device',
    '编码器': 'Encoder',
    '解码器': 'Decoder',
    '格式 / 分辨率 / 帧率': 'Format / Resolution / FPS',
    '-- 扫描中 --': '-- scanning --',
    '-- 请选择设备 --': '-- select a device --',
    '无可用摄像头': 'No camera available',
    '扫描失败': 'Scan failed',
    'Software (libx264)': 'Software (libx264)',
    'Hardware (RKMPP/VENC)': 'Hardware (RKMPP/VENC)',

    /* ---- DBC page ---- */
    '最近解析的 CAN 信号': 'Recently decoded CAN signals',
    '最近发送的 CAN 信号（发送方向）': 'Recently sent CAN signals (TX)',
    '通道': 'Channel',
    '暂无解析数据（请确认已配置 dbc_path 且总线上有匹配报文）': 'No decoded data (confirm dbc_path is configured and matching messages are on the bus)',

    /* ---- logs / recordings page ---- */
    '日志与录像': 'Logs & Recordings',
    '日志': 'Logs',
    '录像': 'Recordings',
    '打包下载': 'Download all',
    '清空日志': 'Clear logs',
    '清空录像': 'Clear recordings',
    '暂无日志文件': 'No log files',
    '暂无录像文件': 'No recording files',

    /* ---- auth.js ---- */
    '需要管理员权限': 'Administrator access required',
    '请输入 root 或 sudo 组成员账号': 'Enter a root or sudo group member account',
    '请输入用户名': 'Please enter a username',
    '用户名或密码含无法编码的字符': 'Username or password contains unencodable characters',
    '用户名或密码错误，请重试': 'Incorrect username or password, please try again',
    '登录失败': 'Sign-in failed',
    '请输入用户名和密码': 'Please enter username and password',
    '会话已过期，请重新登录': 'Session expired, please sign in again',

    /* ---- config.js / common status messages ---- */
    '请先选择 DBC 文件': 'Please select a DBC file first',
    '✓ DBC 已上传（': '✓ DBC uploaded (',
    '上传失败': 'Upload failed',
    '请求失败': 'Request failed',
    '加载失败': 'Load failed',
    '删除失败': 'Delete failed',
    '清空失败': 'Clear failed',
    '✓ 已保存': '✓ Saved',
    '保存失败': 'Save failed',
    '导出失败': 'Export failed',
    '请先选择模型文件': 'Please select a model file first',
    '请先选择类别标签文件': 'Please select a label file first',
    '✓ 已上传并热生效': '✓ Uploaded and applied',
    '✓ 配置已导出': '✓ Config exported',
    '配置 IP 需要先选择绑定网卡': 'Select a bind interface before configuring IP',
    '静态模式需要填写 IP 地址与子网掩码': 'Static mode requires an IP address and subnet mask',
    '最低频率不能大于最高频率': 'Minimum frequency cannot exceed maximum frequency',
    '请输入目标地址': 'Please enter a target address',

    /* ---- config.js CAN ---- */
    '报文 / ': 'messages / ',
    ' 信号）': ' signals)',

    /* ---- config.js YOLO threads ---- */
    '（每核 ': ' (per core ',
    ' 个）': ')',
    '（NPU ': '(NPU ',
    ' 核）': ' cores)',

    /* ---- config.js IP status ---- */
    '当前 ': 'Current ',
    '无 IP': 'no IP',
    ' 网关 ': ' gateway ',

    /* ---- config.js network confirm ---- */
    '将把 ': 'This will set ',
    ' 设为静态 IP ': ' to static IP ',
    '，网关 ': ', gateway ',
    '。\n若当前页面通过该网卡访问，连接会断开，请用新 IP 重新访问。': '.\nIf the current page is accessed via this interface, the connection will be lost. Please reconnect using the new IP.',
    ' 切换到 DHCP 自动获取。\nIP 变化后请用新地址重新访问。': ' to DHCP auto-assign.\nPlease reconnect using the new address after the IP changes.',

    /* ---- config.js CPU governor ---- */
    '性能模式': 'Performance',
    '按需调频': 'On-demand',
    '调度器驱动': 'Schedutil',
    '省电模式': 'Powersave',
    '保守模式': 'Conservative',
    '用户空间': 'Userspace',
    ' 范围 ': ' Range ',
    ' 策略 ': ' Governor ',

    /* ---- config.js network test ---- */
    '✓ 可达': '✓ Reachable',
    '丢包': 'Loss',
    '延迟': 'Latency',
    '✗ 不可达': '✗ Unreachable',
    '✓ 下载速度': '✓ Download speed',
    '✗ 测速失败': '✗ Speed test failed',
    '✓ 网站可达': '✓ Site reachable',
    '✗ 网站不可达': '✗ Site unreachable',
    '⏳ 测试中...': '⏳ Testing...',
    '测试超时': 'Test timed out',
    '⏳ 启动测试...': '⏳ Starting test...',

    /* ---- logs.js ---- */
    '下载': 'Download',
    '录制中': 'Recording',
    '截断并续录': 'Split recording',
    '保存当前录像文件并继续录制到新文件': 'Save the current file and continue recording into a new file',
    '当前录像需先截断保存后下载': 'Split and save the active recording before downloading',
    '截断并续录后下载当前录像': 'Save the current segment, continue recording, and download it',
    '录像下载失败，请重试': 'Unable to download the recording. Please retry',
    '正在截断...': 'Splitting...',
    '当前没有正在录制的视频': 'No active recording',
    '录像状态已变化，请刷新后重试': 'The recording changed; refresh and try again',
    '截断失败 ({status})': 'Split failed ({status})',
    '录像分段保存失败，请检查磁盘与日志': 'Unable to save the recording segment; check disk space and logs',
    '已保存 {file}，正在继续录制': 'Saved {file}; recording continues',
    '录像已停止，请检查设备状态': 'Recording stopped; check the device status',
    '分段请求已提交，正在等待关键帧': 'Split requested; waiting for a keyframe',
    '截断请求失败': 'Split request failed',
    '删除': 'Delete',
    '正在录制的文件无法删除': 'Cannot delete a file that is being recorded',
    '加载中…': 'Loading...',
    '加载失败（需管理员权限）': 'Load failed (admin access required)',
    '需要管理员权限（root 或 sudo 用户）': 'Administrator access required (root or sudo user)',
    '该文件正在录制中，无法删除': 'This file is being recorded and cannot be deleted',
    '打包中…': 'Packing...',
    '打包完成，开始下载': 'Packing complete, starting download',
    '打包失败或已取消': 'Packing failed or cancelled',
    '确定清空全部日志？此操作不可恢复': 'Clear all logs? This cannot be undone',
    '确定清空全部录像？此操作不可恢复': 'Clear all recordings? This cannot be undone',
    '已清空日志 ': 'Cleared ',
    '已清空录像 ': 'Cleared ',
    ' 个日志文件': ' log files',
    ' 个录像文件': ' recording files',
    '删除文件 ': 'Delete file ',
    '？': '?',
    '已删除 ': 'Deleted ',

    /* ---- dbc.js ---- */
    '条记录，每 0.5s 自动刷新': ' records, auto-refresh every 0.5s',

    /* ---- configuration labels and browser-independent file controls ---- */
    'CPU 频率': 'CPU Frequency',
      'CPU 策略组': 'CPU policy group',
      '✓ 已保存，重启后自动恢复': '✓ Saved; restored automatically after restart',
      '{policy} · CPU {cpus} · 当前 {current} MHz · 实际范围 {min}–{max} MHz · 设置范围 {requestedMin}–{requestedMax} MHz': '{policy} · CPU {cpus} · Current {current} MHz · Effective range {min}–{max} MHz · Requested range {requestedMin}–{requestedMax} MHz',
    '调度策略': 'Governor',
    '最低频率': 'Minimum frequency',
    '最高频率': 'Maximum frequency',
    '网络测试': 'Network Tests',
    'Ping 测试': 'Ping test',
    '网速测试': 'Speed test',
    '网站可达性': 'Website reachability',
    '测试': 'Test',
    '测速': 'Test speed',
    '检测': 'Check',
    'OVH (欧洲)': 'OVH (Europe)',
    '自定义地址': 'Custom URL',
    'IP / 域名': 'IP / hostname',
    '选择文件': 'Choose file',
    '未选择文件': 'No file selected',
    '无法执行 ping 命令': 'Unable to run ping',
    '无法解析测速结果': 'Unable to parse speed test results',
    '下载失败，请检查网络连接': 'Download failed; check the network connection',
    '无法执行 curl 命令': 'Unable to run curl',
    '网络测试失败': 'Network test failed',
    '{threads}（每核 {perCore} 个）': '{threads} (per core {perCore})',
    '（NPU {cores} 核）': '(NPU {cores} cores)',
    '当前 {interface}  无 IP': 'Current {interface}: no IP',
    '当前 {interface}  IP {address}{mask}': 'Current {interface}: IP {address}{mask}',
    '当前 {interface}  IP {address}{mask} 网关 {gateway}': 'Current {interface}: IP {address}{mask}, gateway {gateway}',
    '当前 {current} MHz 范围 {min} - {max} MHz 策略 {governor}': 'Current {current} MHz, range {min} - {max} MHz, governor {governor}',
    '✓ DBC 已上传（{messages} 报文 / {signals} 信号）': '✓ DBC uploaded ({messages} messages / {signals} signals)',
    '上传失败 ({status})': 'Upload failed ({status})',
    '请求失败 ({status})': 'Request failed ({status})',
    '保存失败 ({status})': 'Save failed ({status})',
    '导出失败 ({status})': 'Export failed ({status})',
    '删除失败 ({status})': 'Delete failed ({status})',
    '清空失败 ({status})': 'Clear failed ({status})',
    '📦 打包下载': '📦 Download all',
    '{interface} (无 CAN-FD)': '{interface} (no CAN-FD)',
    '✓ 已上传并热生效 ({classes} classes)': '✓ Uploaded and applied ({classes} classes)',
    '已删除 {file}': 'Deleted {file}',
    '已清空 {count} 个日志文件': 'Cleared {count} log files',
    '已清空 {count} 个录像文件': 'Cleared {count} recording files',
    '清空识别结果': 'Clear detections',
    '确定清空全部识别结果？此操作不可恢复，后续检测将继续创建当天文件。': 'Clear all detection reports? This cannot be undone. New detections will create today\'s report again.',
    '已清空 {count} 个识别结果文件': 'Cleared {count} detection report files',
    '清空识别结果失败，请重试': 'Unable to clear detection reports. Please try again.',
    '暂无可打包的识别结果': 'No detection reports to archive',
    '{count}条记录，每 0.5s 自动刷新': '{count} records, auto-refresh every 0.5s'
  };

  window.I18N_EN = EN;

  window.getLang = function () {
    try { return localStorage.getItem('lang') || 'zh'; } catch (e) { return 'zh'; }
  };

  window.t = function (s, values) {
    if (s == null) return s;
    var result = s;
    if (getLang() === 'en') {
      var v = EN[s];
      if (v != null) result = v;
    }
    if (values) result = result.replace(/\{(\w+)\}/g, function (match, key) {
      return Object.prototype.hasOwnProperty.call(values, key) ? String(values[key]) : match;
    });
    return result;
  };

  window.setI18nText = function (el, key, values) {
    if (!el) return;
    el.setAttribute('data-i18n', key);
    if (values) el.setAttribute('data-i18n-values', JSON.stringify(values));
    else el.removeAttribute('data-i18n-values');
    el.textContent = t(key, values);
  };

  window.applyI18n = function (root) {
    root = root || document;
    var els = root.querySelectorAll('[data-i18n]');
    for (var i = 0; i < els.length; i++) {
      var k = els[i].getAttribute('data-i18n');
      var values = null;
      try { values = JSON.parse(els[i].getAttribute('data-i18n-values')); } catch (e) {}
      els[i].textContent = t(k, values);
    }
    els = root.querySelectorAll('[data-i18n-ph]');
    for (var j = 0; j < els.length; j++) els[j].setAttribute('placeholder', t(els[j].getAttribute('data-i18n-ph')));
    els = root.querySelectorAll('[data-i18n-title]');
    for (var m = 0; m < els.length; m++) els[m].setAttribute('title', t(els[m].getAttribute('data-i18n-title')));
    els = root.querySelectorAll('[data-i18n-aria]');
    for (var n = 0; n < els.length; n++) els[n].setAttribute('aria-label', t(els[n].getAttribute('data-i18n-aria')));
  };

  window.updateLangBtn = function () {
    var b = document.getElementById('langBtn');
    if (b) b.textContent = getLang() === 'en' ? 'Chinese' : 'English';
  };

  window.setLang = function (l) {
    try { localStorage.setItem('lang', l); } catch (e) {}
    document.documentElement.setAttribute('lang', l === 'en' ? 'en' : 'zh');
    applyI18n();
    updateLangBtn();
    if (typeof window.onLangChange === 'function') {
      try { window.onLangChange(l); } catch (e) {}
    }
    document.dispatchEvent(new CustomEvent('languagechange', { detail: { lang: l } }));
  };

  window.toggleLang = function () {
    setLang(getLang() === 'en' ? 'zh' : 'en');
  };

  function boot() {
    document.documentElement.setAttribute('lang', getLang() === 'en' ? 'en' : 'zh');
    applyI18n();
    updateLangBtn();
    if (typeof window.onLangChange === 'function') {
      try { window.onLangChange(getLang()); } catch (e) {}
    }
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})();
