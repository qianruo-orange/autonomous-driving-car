// SPDX-License-Identifier: GPL-3.0-or-later
/* i18n.js - bilingual UI (zh / en) with a language toggle.
   The Chinese text is used as the lookup key: in zh mode t() returns it unchanged,
   in en mode t() returns the English string from the dictionary (falling back to
   the Chinese text when a translation is missing). */
(function () {
  var EN = {
    /* ---- brand / navigation / common ---- */
    '边缘智能网关': 'Edge AI Gateway',
    '配置': 'Configuration',
    '仪表盘': 'Dashboard',
    'DBC解析': 'DBC Decoder',
    'DBC 信号解析': 'DBC Signal Decoder',
    '文件下载': 'Files',
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
    '星期日': 'Sunday', '星期一': 'Monday', '星期二': 'Tuesday',
    '星期三': 'Wednesday', '星期四': 'Thursday', '星期五': 'Friday', '星期六': 'Saturday',
    '确定要重启设备吗？': 'Are you sure you want to reboot the device?',
    '确定要关闭设备吗？': 'Are you sure you want to shut down the device?',

    /* ---- AI detection card ---- */
    'AI 检测': 'AI Detection',
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

    /* ---- config.js AI threads ---- */
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
    if (b) b.textContent = getLang() === 'en' ? 'ZH' : 'EN';
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
