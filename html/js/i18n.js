// SPDX-License-Identifier: GPL-3.0-or-later
/* i18n.js - bilingual UI (zh / en) with a language toggle.
   The Chinese text is used as the lookup key: in zh mode t() returns it unchanged,
   in en mode t() returns the English string from the dictionary (falling back to
   the Chinese text when a translation is missing). */
(function () {
  var EN = {
    '目标已过期或丢失，请重新选择': 'Target expired or lost. Select it again',
    '连接失败': 'Connection failed',
    '查看详情': 'Show details',
    '未知错误': 'Unknown error',
    '图像目标追踪': 'Image target tracking',
    '追踪目标': 'Track target',
    '点击画面中的识别框开始追踪': 'Click a detection box in the video to track it',
    '正在追踪所选目标，再次点击取消': 'Tracking the selected target; click it again to stop',
    '目标丢失，请重新选择': 'Target lost. Select it again',
    '等待实时识别画面': 'Waiting for live detections',
    '正在更新追踪目标…': 'Updating tracking target...',
    '无法更新追踪目标': 'Unable to update tracking target',
    '无法读取图像追踪状态': 'Unable to read image tracking status',
    'Web网卡': 'Web interface',
    '选择网卡': 'Select interface',
    '通信状态': 'Connection',
    '停用匿名助手': 'Off',
    '启用匿名助手': 'On',
    '匿名助手端口': 'ANO port',
    '发送频率': 'Send rate',
    '链路测试': 'Link test',
    '关闭测试（实际数据）': 'Off (actual data)',
    '模拟波形': 'Simulated waveforms',
    '正在保存匿名助手设置…': 'Saving ANO settings...',
    '无法读取匿名助手状态': 'Unable to read ANO status',
    '无法保存匿名助手设置，请检查端口是否被占用': 'Unable to save ANO settings; check whether the port is in use',
    '积分限幅': 'Integral limit',
    '限制积分项输出，范围 0–1；0 禁用积分': 'I-term output limit 0–1; 0 disables integration',
    '自动调速': 'Automatic speed control',
    '手动调速': 'Manual speed control',
    '转向参数': 'Steering parameters',
    '最大转向修正量': 'Maximum steering correction',
    '比例系数': 'Proportional coefficient',
    '积分系数': 'Integral coefficient',
    '微分系数': 'Derivative coefficient',
    '跟踪设置': 'Tracking settings',
    '速度策略': 'Speed Strategy',
    '自动设定速度': 'Automatically set speed',
    '智能跟随':'Intelligent Following',
    '控制状态':'Controller state','速度模式':'Speed mode','跟踪目标':'Tracking target',
    '最大轮速':'Maximum wheel speed','手动目标速度':'Manual target speed',

    '中心误差死区':'Center error deadband',

    '停止':'Stopped','启用跟随':'Enable following','手动设定速度':'Manual speed',
    '已保存的目标':'Saved target','目标丢失':'Target lost',
    '暂无识别目标，请将目标置于摄像头画面内':'No detections. Place the target in the camera view',

'中心误差':'Center error',

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
    '相机配置': 'Camera settings',
    '分辨率': 'Resolution',
    '帧率': 'Frame rate',
    '结果': 'Results',
    '清空结果': 'Clear results',
    '结果正在更新，请稍后重试': 'Results are updating; please retry shortly',
    '结果下载失败，请重试': 'Unable to download the result. Please retry',
    '暂无结果文件': 'No result files',
    '暂无可打包的结果': 'No results to archive',
    '确定清空全部结果？此操作不可恢复，后续检测将继续创建当天文件。': 'Clear all results? This cannot be undone. New detections will create today\'s report again.',
    '已清空 {count} 个结果文件': 'Cleared {count} result files',
    '清空结果失败，请重试': 'Unable to clear results. Please try again.',
    '删除日报 {file}？后续检测将继续创建当天文件。': 'Delete report {file}? New detections will recreate today\'s file.',
    '视频采集': 'Video capture',
    '相机设备': 'Camera device',
    '分辨率与帧率': 'Resolution / frame rate',
    '无法读取相机能力，请重试': 'Unable to read camera capabilities. Please retry',
    '正在读取相机配置…': 'Loading camera settings...',
    '无法读取相机配置，请重试': 'Unable to load camera settings. Please retry',
    '正在保存相机配置…': 'Saving camera settings...',
    /* ---- brand / navigation / common ---- */
    '自动驾驶小车': 'Autonomous Driving Car',
    '系统配置': 'System Configuration',
    '仪表盘': 'Dashboard',
    '主界面': 'Home',
    '系统状态': 'System Status',
    'DBC解析': 'DBC Decoder',
    'DBC 信号解析': 'DBC Signal Decoder',
    '文件下载': 'Files & downloads',
    '主题': 'Theme',
    '切换主题': 'Toggle theme',
    '切换语言': 'Switch language',
    '重启': 'Reboot',
    '关机': 'Shutdown',
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
    '加速器': 'Accelerator',
    '存储': 'Storage',
    '内存': 'Memory',
    '磁盘': 'Disk',
    '网络速度': 'Network',
    '有线': 'Wired',
    '无线': 'Wireless',
    '已连接': 'Connected',
    '未连接': 'Disconnected',
    '采集 {camera} FPS · 处理 {processing} ms · 端到端 {delay} ms · 丢帧 {dropped}/s': 'Capture {camera} FPS · Processing {processing} ms · End-to-end {delay} ms · Dropped {dropped}/s',
    '星期日': 'Sunday', '星期一': 'Monday', '星期二': 'Tuesday',
    '星期三': 'Wednesday', '星期四': 'Thursday', '星期五': 'Friday', '星期六': 'Saturday',
    '确定要重启设备吗？': 'Are you sure you want to reboot the device?',
    '确定要关闭设备吗？': 'Are you sure you want to shut down the device?',

    /* ---- YOLO detection card ---- */
    'YOLO 检测': 'YOLO Detection',
    '推理参数': 'Inference',
    '置信度': 'Confidence',
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
    'Web与匿名助手设置': 'Web & ANO Settings',
    '绑定网卡': 'Bind interface',
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

    /* ---- DBC page ---- */
    '最近解析的 CAN 信号': 'Recently decoded CAN signals',
    '最近发送的 CAN 信号（发送方向）': 'Recently sent CAN signals (TX)',
    '通道': 'Channel',
    '暂无解析数据（请确认已配置 dbc_path 且总线上有匹配报文）': 'No decoded data (confirm dbc_path is configured and matching messages are on the bus)',

    /* ---- logs / recordings page ---- */
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
    ' 核）': ' cores)',

    /* ---- config.js IP status ---- */
    '当前 ': 'Current ',

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

    /* ---- config.js network test ---- */
    '✓ 可达': '✓ Reachable',
    '丢包': 'Loss',
    '延迟': 'Latency',
    '✗ 不可达': '✗ Unreachable',
    '✓ 下载速度': '✓ Download speed',
    '✗ 测速失败': '✗ Speed test failed',
    '⏳ 测试中...': '⏳ Testing...',
    '测试超时': 'Test timed out',
    '⏳ 启动测试...': '⏳ Starting test...',

    /* ---- logs.js ---- */
    '下载': 'Download',
    '录制中': 'Recording',
    '截断并续录': 'Split recording',
    '截断并续录后下载当前录像': 'Save the current segment, continue recording, and download it',
    '录像下载失败，请重试': 'Unable to download the recording. Please retry',
    '正在截断...': 'Splitting...',
    '录像状态已变化，请刷新后重试': 'The recording changed; refresh and try again',
    '截断失败 ({status})': 'Split failed ({status})',
    '录像分段保存失败，请检查磁盘与日志': 'Unable to save the recording segment; check disk space and logs',
    '已保存 {file}，正在继续录制': 'Saved {file}; recording continues',
    '录像已停止，请检查设备状态': 'Recording stopped; check the device status',
    '分段请求已提交，正在等待关键帧': 'Split requested; waiting for a keyframe',
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
      '{policy} · CPU {cpus} · 当前 {current} MHz · 实际 {min}–{max} MHz · 设置 {requestedMin}–{requestedMax} MHz': '{policy} · CPU {cpus} · Current {current} MHz · Effective range {min}–{max} MHz · Requested range {requestedMin}–{requestedMax} MHz',
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
    '网络测试失败': 'Network test failed',
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
    '{count}条记录，每 0.5s 自动刷新': '{count} records, auto-refresh every 0.5s',
    "Codex 对话": "Codex chat",
    "未登录": "Not signed in",
    "登录": "Sign in",
    "已登录": "Signed in",
    "等待授权": "Waiting for authorisation",
    "等待浏览器授权": "Waiting for browser authorisation",
    "沙箱": "Sandbox",
    "会话": "Session",
    "记录": "Records",
    "个文件": "files",
    "请先在配置页登录 Codex": "Sign in to Codex on the configuration page first",
    "出错": "Error",
    "Codex 正在思考": "Codex is thinking",
    "还没有对话。输入一条消息开始和 Codex 交流。": "No conversation yet. Send a message to start chatting with Codex.",
    "发送失败": "Send failed",
    "已停止生成": "Generation stopped",
    "停止失败": "Could not stop",
    "记录已开始下载": "Download started",
    "记录下载失败": "Could not download records",
    "输入消息，Enter 发送，Shift+Enter 换行": "Type a message; Enter sends, Shift+Enter adds a line",
    "Codex CLI 配置": "Codex CLI settings",
    "模型": "Model",
    "推理速率": "Reasoning effort",
    "默认（跟随 CLI）": "Default (follow the CLI)",
    "极低（minimal）": "Minimal",
    "低（low）": "Low",
    "中（medium，推荐）": "Medium (recommended)",
    "高（high）": "High",
    "极高（xhigh，仅 codex-max / 5.2）": "Extra high (codex-max / 5.2 only)",
    "已保存 Codex 设置": "Codex settings saved",
    "Codex 设置保存失败": "Could not save Codex settings",
    "正在获取登录链接…": "Getting the sign-in link…",
    "打开登录链接，输入下面的一次性验证码完成授权（15 分钟内有效）。": "Open the link and enter the one-time code below (valid for 15 minutes).",
    "正在获取一次性设备码，拿到后打开链接输入即可完成授权。": "Fetching a one-time device code; open the link and enter it to finish sign-in.",
    "登录完成，可以回到主界面开始对话。": "Signed in. Return to the main page to chat.",
    "设备码登录失败，请重试": "Device-code sign-in failed; please try again",
    "复制验证码": "Copy code",
    "验证码已复制": "Code copied",
    "复制失败": "Copy failed",
    "已发起设备码登录": "Device-code sign-in started",
    "发起登录失败": "Could not start sign-in",
    "退出登录": "Sign out",
    "确定退出 Codex 登录并清除已保存的凭据？": "Sign out of Codex and remove the saved credentials?",
    "已退出登录": "Signed out",
    "退出登录失败": "Could not sign out",
    "未找到 Codex CLI": "Codex CLI not found",
    "检查更新": "Check for updates",
    "检查中…": "Checking…",
    "检查更新失败": "Update check failed",
    "当前 {current} · 最新 {latest}": "Current {current} · latest {latest}",
    "有可用更新": "Update available",
    "已是最新版本": "Already up to date",
    "在板端执行 codex update 升级": "Run codex update on the board to upgrade",
    "使用上次检查结果": "Showing the last successful check",
    "安装更新": "Install update",
    "正在安装更新，请勿断电…": "Installing — do not power off…",
    "已更新到 {version}": "Updated to {version}",
    "安装失败": "Installation failed",
    "已在后台开始更新": "Update started in the background",
    "正在下载更新 {percent}%": "Downloading update {percent}%",
    "正在启动安装…": "Starting the update…",
    "上次尝试失败": "last attempt failed",
    "已在后台开始更新（约 190 MB，约 10 分钟以上）；完成后刷新页面查看版本": "Update started in the background (~190 MB, may take a while); reload later to see the version",
    "可查看板端日志重试": "check the board logs and retry",
    "有可用更新，点击安装更新": "Update available — click Install update",
    "额度已用尽": "Quota exhausted",
    "可用额度": "Available quota",
    "代理开关": "Proxy switch",
    "代理已关闭，流量直连": "Proxy off, traffic goes direct",
    "每周额度 {time} 重置": "Weekly quota resets {time}",
    "{label} {time} 重置": "{label} resets {time}",
    "{hours} 小时": "{hours} hours",
    "{days} 天": "{days} days",
    "短期额度": "Short window",
    "长期额度": "Long window",
    "余额 {balance}": "Balance {balance}",
    "可用重置次数 {count}": "Reset credits available: {count}",
    "模型列表读取失败": "Cannot read the model catalog",
    "当前已保存": "currently saved",
    "推荐": "recommended",
    "默认（跟随 CLI）": "Default (follow CLI)",
    "5 小时": "5 hours",
    "每周": "Weekly",
    "极低": "Minimal",
    "低": "Low",
    "中": "Medium",
    "高": "High",
    "极高": "Extra high",
    "最大": "Max",
    "极限": "Ultra",
    "Codex": "Codex",
    "运行日志": "Run logs",
    "检测录像": "Detection recordings",
    "检测结果": "Detection results",
    "Codex 记录": "Codex records",
    "创建时间": "Created",
    "清空记录": "Clear records",
    "暂无交互记录": "No interaction records",
    "确定清空全部 Codex 交互记录？此操作不可恢复。": "Delete all Codex interaction records? This cannot be undone.",
    "已清空 {count} 个记录文件": "Cleared {count} record files",
    "清空记录失败": "Could not clear records",
    "打包失败": "Could not pack records",
    "完整交互记录已开始下载": "Full interaction log download started",
    "发送": "Send",
    "网络代理与测试": "Network proxy & tests",
    "代理主机": "Proxy host",
    "端口": "Port",
    "协议": "Protocol",
    "不走代理的地址": "Bypass list",
    "当前代理地址": "Effective proxy URL",
    "保存配置": "Save config",
    "保存并应用": "Save and apply",
    "重新读取": "Reload",
    "原始文件内容": "Raw config file",
    "配置文件": "Config file",
    "服务状态": "Service",
    "系统环境变量": "System environment",
    "影响整机网络流量（保存并应用后生效）": "Applies to the whole board after Save and apply",
    "未配置": "Not set",
    "配置文件不存在": "Config file not found",
    "读取失败": "Load failed",
    "已保存": "Saved",
    "已保存并应用": "Saved and applied",
    "已保存，但应用失败": "Saved, but applying failed",
    "已保存（需保存并应用才能对整机生效）": "Saved; use Save and apply to update the whole board",
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
    if (b) b.textContent = getLang() === 'en' ? '简体中文' : 'English';
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
