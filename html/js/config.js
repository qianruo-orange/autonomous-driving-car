// SPDX-License-Identifier: GPL-3.0-or-later
/* config.js — 配置页面逻辑 */
(function() {

  var curCan = 0;      /* 当前选中的 CAN 通道索引 */
  var canNames = [];   /* 通道名列表（由 /api/config 动态读取） */
  var cansCfg = {};    /* 各通道配置缓存 { can0:{...}, ... } */
  var canFD = {};      /* 系统接口 CAN FD 支持映射 { can0: 1, ... } */

  function curName() { return canNames[curCan] || ('can' + curCan); }

  function chkFd() {
    document.getElementById('dbr').style.display = document.getElementById('fd').value === 'on' ? 'flex' : 'none';
  }

  function cardStatus(cardId, type, msg, values) {
    var el = document.getElementById('status_' + cardId);
    if (!el) return;
    el.className = 'card-status ' + type;
    setI18nText(el, msg, values);
    if (type === 'ok') setTimeout(function() { el.className = 'card-status'; setI18nText(el, ''); }, 3000);
    else setTimeout(function() { el.className = 'card-status'; setI18nText(el, ''); }, 5000);
  }

  function updateFileName(input) {
    var label = document.querySelector('[data-file-name="' + input.id + '"]');
    if (!label) return;
    if (input.files && input.files.length) {
      label.removeAttribute('data-i18n');
      label.textContent = input.files[0].name;
    } else setI18nText(label, '未选择文件');
  }
  document.querySelectorAll('[data-file-picker]').forEach(function(button) {
    var input = document.getElementById(button.getAttribute('data-file-picker'));
    button.addEventListener('click', function() { input.click(); });
    input.addEventListener('change', function() { updateFileName(input); });
  });

  function setSwitch(up) {
    document.getElementById('st').textContent = up ? '\ud83d\udfe2' : '\ud83d\udd34';
    document.getElementById('tg').checked = up;
    document.getElementById('cfg').style.display = up ? 'block' : 'none';
    document.getElementById('hint').style.display = up ? 'none' : 'block';
  }

  function setFilters(filters) {
    var el = document.getElementById('fil');
    el.innerHTML = '';
    if (!filters) return;
    filters.forEach(function(f) {
      var r = document.createElement('div');
      r.className = 'filter-row';
      r.innerHTML = '<input type="text" value="' + f.id + '" placeholder="ID (hex)">'
        + '<input type="text" value="' + f.mask + '" placeholder="Mask (hex)">'
        + '<button type="button" class="del-btn" onclick="this.parentElement.remove()">\u2715</button>';
      el.appendChild(r);
    });
  }

  /* 把当前表单内容写入 cansCfg[curName()] */
  function collectCan() {
    var f = cansCfg[curName()] || (cansCfg[curName()] = {});
    f.bitrate = document.getElementById('br').value;
    f.fd = document.getElementById('fd').value;
    f.dbitrate = document.getElementById('db').value;
    f.up = document.getElementById('tg').checked ? 'on' : 'off';
    f.filters = [];
    var rows = document.getElementById('fil').children;
    for (var k = 0; k < rows.length; k++) {
      var inputs = rows[k].querySelectorAll('input');
      if (inputs.length >= 2) f.filters.push({id: inputs[0].value, mask: inputs[1].value});
    }
  }

  /* 用 cansCfg[curName()] 渲染当前通道 */
  function renderCan() {
    var f = cansCfg[curName()] || {};
    document.getElementById('br').value = f.bitrate || '';
    document.getElementById('fd').value = f.fd || 'off';
    document.getElementById('db').value = f.dbitrate || '';
    setSwitch(f.up === 'on');
    chkFd();
    setFilters(f.filters || []);
    document.getElementById('dbc').value = '';
    updateFileName(document.getElementById('dbc'));
  }

  window.onCanSel = function() {
    collectCan();
    curCan = parseInt(document.getElementById('canSel').value, 10) || 0;
    renderCan();
  };

  window.toggleCan = async function(on) {
    var n = curName();
    var tg = document.getElementById('tg');
    tg.disabled = true;
    try {
      await authFetch('/api/can/toggle', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'ifname=' + n + '&action=' + (on ? 'up' : 'down')
      });
    } catch(e) {}
    setSwitch(on);
    cansCfg[n] = cansCfg[n] || {};
    cansCfg[n].up = on ? 'on' : 'off';
    tg.disabled = false;
  };

  window.chkFd = chkFd;

  window.uploadDbc = async function() {
    var input = document.getElementById('dbc');
    var file = input.files && input.files[0];
    if (!file) { cardStatus('can', 'err', '请先选择 DBC 文件'); return; }
    var n = curName();
    try {
      var r = await authFetch('/api/can/dbc?ifname=' + n, {
        method: 'POST',
        headers: {'Content-Type': 'application/octet-stream'},
        body: file
      });
      var j = r.ok ? await r.json() : null;
      if (j && j.result === 'ok') {
        cardStatus('can', 'ok', '✓ DBC 已上传（{messages} 报文 / {signals} 信号）', {messages: j.messages, signals: j.signals});
      } else {
        cardStatus('can', 'err', '上传失败 ({status})', {status: r.status});
      }
    } catch(e) { cardStatus('can', 'err', '上传失败'); }
  };

  window.addFilter = function() {
    var el = document.getElementById('fil');
    if (el.children.length >= 16) return;
    var r = document.createElement('div');
    r.className = 'filter-row';
    r.innerHTML = '<input type="text" placeholder="ID (hex)">'
      + '<input type="text" placeholder="Mask (hex)">'
      + '<button type="button" class="del-btn" onclick="this.parentElement.remove()">\u2715</button>';
    el.appendChild(r);
  };

  /* 加载网络接口列表，填充"绑定网卡"下拉框 */
  async function loadIfaces() {
    var sel = document.getElementById('bind');
    sel.innerHTML = '<option value="" data-i18n="所有网卡">' + t('所有网卡') + '</option>';
    try {
      var r = await fetch('/api/network/ifaces');
      if (!r.ok) return;
      var list = await r.json();
      for (var i = 0; i < list.length; i++)
        sel.innerHTML += '<option value="' + list[i] + '">' + list[i] + '</option>';
    } catch(e) {}
  }

  /* 枚举系统全部 CAN 接口及其 CAN FD 支持情况 */
  async function loadCanIfaces() {
    try {
      var r = await fetch('/api/can/ifaces');
      if (!r.ok) return;
      var list = await r.json();
      canFD = {};
      for (var i = 0; i < list.length; i++) canFD[list[i].name] = list[i].fd;
    } catch(e) {}
  }

  async function loadCfg() {
    try {
      var cfg = await (await fetch('/api/config')).json();
      document.getElementById('tp').value = cfg.tcp_port || '';
      document.getElementById('mc').value = cfg.max_clients || '';
      document.getElementById('bind').value = cfg.tcp_bind || '';

      /* AI thresholds are percentages; concurrency and pacing are automatic. */
      document.getElementById('ai_cf').value = Math.round((cfg.ai_conf || 0.25) * 100);
      document.getElementById('ai_nm').value = Math.round((cfg.ai_nms || 0.45) * 100);
      /* IP 设置：IP 即 TCP 绑定网卡的 IP；回填保存配置 + 当前运行时地址 */
      document.getElementById('ip_mode').value = cfg.ip_mode || '';
      document.getElementById('ip_a').value = cfg.ip_addr || '';
      document.getElementById('ip_m').value = cfg.ip_mask || '';
      document.getElementById('ip_g').value = cfg.ip_gw || '';
      var ipKey = !cfg.ip_cur_addr ? '当前 {interface}  无 IP' :
        cfg.ip_cur_gw ? '当前 {interface}  IP {address}{mask} 网关 {gateway}' : '当前 {interface}  IP {address}{mask}';
      setI18nText(document.getElementById('ip_cur'), ipKey, {interface: cfg.ip_cur_if || '',
        address: cfg.ip_cur_addr || '', mask: cfg.ip_cur_mask ? ' / ' + cfg.ip_cur_mask : '', gateway: cfg.ip_cur_gw || ''});
      chkIpMode();

      canNames = Object.keys(cfg.cans || {});

      /* 与系统实际枚举的接口合并：枚举接口优先展示，配置中已存在但未枚举到的保留。
         系统无 CAN 接口且配置中也没有通道时，canNames 保持为空，下方显示"无可用"提示。 */
      var sysNames = Object.keys(canFD);
      if (sysNames.length > 0) {
        var merged = sysNames.slice();
        for (var ci = 0; ci < canNames.length; ci++)
          if (merged.indexOf(canNames[ci]) < 0) merged.push(canNames[ci]);
        canNames = merged;
      }

      /* 动态生成通道下拉框（标注 CAN FD 支持） */
      var sel = document.getElementById('canSel');
      sel.innerHTML = '';
      if (canNames.length === 0) {
        sel.innerHTML = '<option value="0" data-i18n="无可用 CAN 接口">' + t('无可用 CAN 接口') + '</option>';
        document.getElementById('cfg').style.display = 'none';
        document.getElementById('hint').style.display = 'block';
        setI18nText(document.getElementById('hint'), '系统未检测到 CAN 设备');
      } else {
        for (var i = 0; i < canNames.length; i++) {
          var nm = canNames[i];
          var tag = '';
          if (canFD[nm] === 1) tag = ' (CAN-FD)';
          var option = document.createElement('option');
          option.value = String(i);
          if (canFD[nm] === 0) setI18nText(option, '{interface} (无 CAN-FD)', {interface: nm});
          else option.textContent = nm + tag;
          sel.appendChild(option);
        }
        setI18nText(document.getElementById('hint'), '接口已关闭，开启后可配置');
      }
      if (curCan >= canNames.length) curCan = 0;
      sel.value = String(curCan);

      cansCfg = {};
      for (var i = 0; i < canNames.length; i++) {
        var f = (cfg.cans || {})[canNames[i]] || {};
        cansCfg[canNames[i]] = {
          bitrate: f.bitrate || '',
          fd: f.fd || 'off',
          dbitrate: f.dbitrate || '',
          up: f.up === 'on' ? 'on' : 'off',
          filters: f.filters || []
        };
      }
      renderCan();
    } catch(e) {}
  }
  window.loadCfg = loadCfg;

  /* 提交一个模块的配置（target 指定模块，后端只应用该模块并重启对应模块） */
  async function postConfig(d, cardId) {
    try {
      var r = await authFetch('/api/config', {method: 'POST', body: new URLSearchParams(d)});
      if (cardId) {
        if (r.ok) cardStatus(cardId, 'ok', '✓ 已保存');
        else cardStatus(cardId, 'err', '请求失败 ({status})', {status: r.status});
      }
      return r.ok;
    } catch(e) {
      if (cardId) cardStatus(cardId, 'err', '请求失败');
      return false;
    }
  }

  /* 保存当前 CAN 通道配置并重新配置该通道 */
  window.saveCan = async function() {
    collectCan();
    var name = canNames[curCan];
    if (!name) { cardStatus('can', 'err', '无可用 CAN 接口'); return; }
    var f = cansCfg[name] || {};
    var d = {target: 'can'};
    d['ifname' + curCan] = name;
    d['bitrate' + curCan] = f.bitrate;
    d['fd' + curCan] = f.fd;
    d['dbitrate' + curCan] = f.dbitrate;
    d['up' + curCan] = f.up;
    var filters = f.filters || [];
    for (var k = 0; k < filters.length; k++) {
      d['filter_id_' + curCan + '_' + k] = filters[k].id;
      d['filter_mask_' + curCan + '_' + k] = filters[k].mask;
    }
    await postConfig(d, 'can');
  };

  /* 保存网络设置：TCP 数据通道参数 + 该网卡的 IP 配置（IP 即 TCP 绑定网卡的 IP） */
  window.saveNet = async function() {
    var mode = document.getElementById('ip_mode').value;
    var ifn = document.getElementById('bind').value;
    if (mode !== '') {
      if (!ifn) { cardStatus('net', 'err', '配置 IP 需要先选择绑定网卡'); return; }
      if (mode === 'static') {
        var a = document.getElementById('ip_a').value.trim();
        var m = document.getElementById('ip_m').value.trim();
        if (!a || !m) { cardStatus('net', 'err', '静态模式需要填写 IP 地址与子网掩码'); return; }
        if (!confirm(t('将把 ') + ifn + t(' 设为静态 IP ') + a + '/' + m
          + (document.getElementById('ip_g').value ? t('，网关 ') + document.getElementById('ip_g').value : '')
          + t('。\n若当前页面通过该网卡访问，连接会断开，请用新 IP 重新访问。'))) return;
      } else if (mode === 'dhcp') {
        if (!confirm(t('将把 ') + ifn + t(' 切换到 DHCP 自动获取。\nIP 变化后请用新地址重新访问。'))) return;
      }
    }
    /* 1) TCP 服务参数 */
    var ok1 = await postConfig({
      target: 'net',
      tcp_port: document.getElementById('tp').value,
      max_clients: document.getElementById('mc').value,
      tcp_bind: ifn
    });
    /* 2) IP 设置（mode 为空 = 关闭不管理，仍保存字段） */
    if (ok1) {
      await postConfig({
        target: 'ip',
        ip_ifname: ifn,
        ip_mode: mode,
        ip_addr: document.getElementById('ip_a').value.trim(),
        ip_mask: document.getElementById('ip_m').value.trim(),
        ip_gw: document.getElementById('ip_g').value.trim()
      }, 'net');
    }
  };

  /* 按 IP 模式显隐静态地址输入行 */
  window.chkIpMode = function() {
    var mode = document.getElementById('ip_mode').value;
    var show = mode === 'static';
    document.getElementById('ip_a_row').style.display = show ? 'flex' : 'none';
    document.getElementById('ip_m_row').style.display = show ? 'flex' : 'none';
    document.getElementById('ip_g_row').style.display = show ? 'flex' : 'none';
  };

  /* Save thresholds; inference concurrency and pacing are automatic. */
  window.saveAi = async function() {
    await postConfig({target:'ai', ai_conf:document.getElementById('ai_cf').value,
      ai_nms:document.getElementById('ai_nm').value}, 'ai');
  };

  window.uploadAi = async function(type) {
    var input = document.getElementById(type === 'model' ? 'ai_model_f' : 'ai_names_f');
    var file = input.files && input.files[0];
    if (!file) { cardStatus('ai', 'err', type === 'model' ? '请先选择模型文件' : '请先选择类别标签文件'); return; }
    try {
      var r = await authFetch('/api/ai/upload?type=' + type, {
        method: 'POST',
        headers: {'Content-Type': 'application/octet-stream'},
        body: file
      });
      var j = r.ok ? await r.json() : null;
      if (j && j.result === 'ok') {
        cardStatus('ai', 'ok', type === 'names' ? '✓ 已上传并热生效 ({classes} classes)' : '✓ 已上传并热生效', {classes: j.classes});
        setTimeout(loadCfg, 800);
      } else if (j) {
        cardStatus('ai', 'err', '上传失败 ({status})', {status: j.result || ('HTTP ' + r.status)});
      } else {
        cardStatus('ai', 'err', '上传失败 ({status})', {status: r.status});
      }
    } catch(e) { cardStatus('ai', 'err', '上传失败'); }
    input.value = '';
    updateFileName(input);
  };

  /* 导出配置：只导出 config 文件夹——后端打包整个 config 目录
     （config.txt / 模型 / DBC / 标签），authFetch 带登录重试，
     成功转 blob 触发浏览器下载 */
  window.exportCfg = async function() {
    try {
      var pr = await authFetch('/api/config/export');
      if (!pr.ok) throw new Error('HTTP ' + pr.status);
      var pb = await pr.blob();
      var pa = document.createElement('a');
      pa.href = URL.createObjectURL(pb);
      pa.download = 'config_pack.tar.gz';
      pa.click();
      setTimeout(function() { URL.revokeObjectURL(pa.href); }, 1000);
      cardStatus('export', 'ok', '✓ 配置已导出');
    } catch(e) {
      cardStatus('export', 'err', '导出失败 ({status})', {status: e.message});
    }
  };

  window.reboot = async function() {
    if (!confirm(t('确定要重启设备吗？'))) return;
    try { await authFetch('/api/reboot'); } catch(e) {}
  };

  window.shutdown = async function() {
    if (!confirm(t('确定要关闭设备吗？'))) return;
    try { await authFetch('/api/shutdown'); } catch(e) {}
  };

  /* 先扫描设备列表、CAN 接口与网卡列表，再加载配置 */
  loadCanIfaces().then(loadIfaces).then(loadCfg).then(loadCpuFreq);

  /* ---- CPU 频率配置 ---- */
  async function loadCpuFreq() {
    try {
      var r = await fetch('/api/cpu_freq');
      if (!r.ok) return;
      var d = await r.json();

      /* 填充 governor 下拉框 */
      var govSel = document.getElementById('cf_gov');
      if (govSel && d.governors) {
        govSel.innerHTML = '';
        var govNames = {
          'performance': '性能模式',
          'ondemand': '按需调频',
          'schedutil': '调度器驱动',
          'powersave': '省电模式',
          'conservative': '保守模式',
          'userspace': '用户空间'
        };
        for (var i = 0; i < d.governors.length; i++) {
          var g = d.governors[i];
          var option = document.createElement('option');
          option.value = g;
          setI18nText(option, govNames[g] || g);
          govSel.appendChild(option);
        }
        govSel.value = d.governor || '';
      }

      /* 填充频率下拉框 */
      var freqs = d.frequencies || [];
      function fillFreqSel(id, curVal) {
        var sel = document.getElementById(id);
        if (!sel) return;
        sel.innerHTML = '';
        for (var i = 0; i < freqs.length; i++) {
          var f = freqs[i];
          var mhz = (f / 1000).toFixed(0);
          sel.innerHTML += '<option value="' + f + '">' + mhz + ' MHz</option>';
        }
        sel.value = String(curVal);
      }
      fillFreqSel('cf_min', d.min_freq);
      fillFreqSel('cf_max', d.max_freq);

      /* 当前状态 */
      var curEl = document.getElementById('cf_cur');
      if (curEl) {
        var curMhz = (d.cur_freq / 1000).toFixed(0);
        var minMhz = (d.min_freq / 1000).toFixed(0);
        var maxMhz = (d.max_freq / 1000).toFixed(0);
        setI18nText(curEl, '当前 {current} MHz 范围 {min} - {max} MHz 策略 {governor}',
          {current: curMhz, min: minMhz, max: maxMhz, governor: d.governor || ''});
      }
    } catch(e) {}
  }

  window.saveCpuFreq = async function() {
    var gov = document.getElementById('cf_gov').value;
    var minF = document.getElementById('cf_min').value;
    var maxF = document.getElementById('cf_max').value;

    if (parseInt(minF) > parseInt(maxF)) {
      cardStatus('cpufreq', 'err', '最低频率不能大于最高频率');
      return;
    }

    try {
      var r = await authFetch('/api/cpu_freq', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'governor=' + encodeURIComponent(gov) +
              '&min_freq=' + encodeURIComponent(minF) +
              '&max_freq=' + encodeURIComponent(maxF)
      });
      if (r.ok) {
        cardStatus('cpufreq', 'ok', '✓ 已保存');
        setTimeout(loadCpuFreq, 500);
      } else {
        cardStatus('cpufreq', 'err', '保存失败 ({status})', {status: 'HTTP ' + r.status});
      }
    } catch(e) {
      cardStatus('cpufreq', 'err', '请求失败');
    }
  };

  /* ---- 网络测试 ---- */
  var ntLatest = {};
  function ntError(error) {
    var message = t(String(error || ''));
    if (getLang() === 'en' && /[\u3400-\u9fff]/.test(message)) message = t('网络测试失败');
    var span = document.createElement('span');
    span.textContent = message;
    return span.innerHTML;
  }
  var ntSpeedSel = document.getElementById('nt_speed_target');
  var ntSpeedCustom = document.getElementById('nt_speed_custom');
  if (ntSpeedSel) {
    ntSpeedSel.addEventListener('change', function() {
      ntSpeedCustom.style.display = this.value === 'custom' ? '' : 'none';
    });
  }

  function ntResultEl(type) {
    return document.getElementById(type === 'ping' ? 'nt_ping_result' :
                                   type === 'speed' ? 'nt_speed_result' :
                                   'nt_reach_result');
  }

  function ntTarget(type) {
    if (type === 'ping') return document.getElementById('nt_ping_target').value.trim();
    if (type === 'speed') {
      var sel = document.getElementById('nt_speed_target');
      if (sel.value === 'custom') return document.getElementById('nt_speed_custom').value.trim();
      return sel.value;
    }
    if (type === 'reach') return document.getElementById('nt_reach_target').value.trim();
    return '';
  }

  function ntRenderPing(el, data) {
    if (!data || !data.result) return;
    var r = data.result;
    if (r.reachable) {
      var html = '<span class="nt-ok">' + t('✓ 可达') + '</span>';
      if (r.packet_loss >= 0) html += ' &nbsp;' + t('丢包') + ' <span class="nt-val">' + r.packet_loss + '%</span>';
      if (r.rtt_avg >= 0) html += ' &nbsp;' + t('延迟') + ' <span class="nt-val">' + r.rtt_avg.toFixed(1) + ' ms</span>';
      if (r.rtt_min >= 0 && r.rtt_max >= 0) html += ' &nbsp;(min ' + r.rtt_min.toFixed(1) + ' / max ' + r.rtt_max.toFixed(1) + ')';
      el.innerHTML = html;
    } else {
      el.innerHTML = '<span class="nt-fail">' + t('✗ 不可达') + '</span>' + (data.error ? ' <span class="nt-fail">' + ntError(data.error) + '</span>' : '');
    }
  }

  function ntRenderSpeed(el, data) {
    if (!data || !data.result) return;
    var r = data.result;
    if (r.speed_mbps !== undefined) {
      var mbps = r.speed_mbps;
      var label = mbps >= 1 ? mbps.toFixed(2) + ' Mbps' : (mbps * 1000).toFixed(1) + ' Kbps';
      el.innerHTML = '<span class="nt-ok">' + t('✓ 下载速度') + '</span> <span class="nt-val">' + label + '</span>'
        + ' &nbsp;(' + (r.size_bytes / 1048576).toFixed(1) + ' MB / ' + r.time_sec.toFixed(1) + 's)';
    } else {
      el.innerHTML = '<span class="nt-fail">' + t('✗ 测速失败') + '</span>' + (data.error ? ' <span class="nt-fail">' + ntError(data.error) + '</span>' : '');
    }
  }

  function ntRenderReach(el, data) {
    if (!data || !data.result) return;
    var r = data.result;
    if (r.reachable) {
      var html = '<span class="nt-ok">' + t('✓ 网站可达') + '</span>';
      if (r.rtt_avg >= 0) html += ' &nbsp;' + t('延迟') + ' <span class="nt-val">' + r.rtt_avg.toFixed(1) + ' ms</span>';
      el.innerHTML = html;
    } else {
      el.innerHTML = '<span class="nt-fail">' + t('✗ 网站不可达') + '</span>' + (data.error ? ' <span class="nt-fail">' + ntError(data.error) + '</span>' : '');
    }
  }

  function ntRender(type, data) {
    ntLatest[type] = data;
    var el = ntResultEl(type);
    if (!el) return;
    if (data.status === 'starting' || data.status === 'pending' || data.status === 'running') {
      el.className = 'nt-result testing';
      el.textContent = t(data.status === 'starting' ? '⏳ 启动测试...' : '⏳ 测试中...');
      return;
    }
    el.className = 'nt-result';
    if (data.status === 'timedout' || data.status === 'failed') {
      el.innerHTML = '<span class="nt-fail">' + (data.status === 'timedout' ? t('测试超时') :
        t('请求失败') + (data.httpStatus ? ' (HTTP ' + data.httpStatus + ')' : '')) + '</span>';
      return;
    }
    if (data.error && !data.result) {
      el.innerHTML = '<span class="nt-fail">✗ ' + ntError(data.error) + '</span>';
      return;
    }
    if (type === 'ping' || type === 'reach') ntRenderPing(el, data);
    else if (type === 'speed') ntRenderSpeed(el, data);
  }

  async function ntPoll(type, testId) {
    var maxAttempts = 30;
    for (var i = 0; i < maxAttempts; i++) {
      await new Promise(function(r) { setTimeout(r, 1000); });
      try {
        var r = await fetch('/api/nettest/result?id=' + testId);
        if (!r.ok) continue;
        var data = await r.json();
        ntRender(type, data);
        if (data.status === 'done') return;
      } catch(e) {}
    }
    ntRender(type, {status: 'timedout'});
  }

  window.runNetTest = async function(type) {
    var target = ntTarget(type);
    if (!target) {
      cardStatus('nettest', 'err', '请输入目标地址');
      return;
    }

    /* 可达性测试复用 ping 后端，从 URL 提取主机名 */
    var apiType = type;
    var apiTarget = target;
    if (type === 'reach') {
      apiType = 'ping';
      try {
        var u = new URL(target);
        apiTarget = u.hostname;
      } catch(e) {
        apiTarget = target.replace(/^https?:\/\//, '').split('/')[0];
      }
    }

    var el = ntResultEl(type);
    ntRender(type, {status: 'starting'});

    try {
      var r = await authFetch('/api/nettest', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'type=' + encodeURIComponent(apiType) + '&target=' + encodeURIComponent(apiTarget)
      });
      if (!r.ok) {
        ntRender(type, {status: 'failed', httpStatus: r.status});
        return;
      }
      var data = await r.json();
      if (data.test_id) ntPoll(type, data.test_id);
    } catch(e) {
      ntRender(type, {status: 'failed'});
    }
  };

  document.addEventListener('languagechange', function() {
    Object.keys(ntLatest).forEach(function(type) { ntRender(type, ntLatest[type]); });
  });

})();
