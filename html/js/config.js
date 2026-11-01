// SPDX-License-Identifier: GPL-3.0-or-later
/* config.js — 配置页面逻辑 */
(function() {

  var curCan = 0;      /* 当前选中的 CAN 通道索引 */
  var canNames = [];   /* 通道名列表（由 /api/config 动态读取） */
  var cansCfg = {};    /* 各通道配置缓存 { can0:{...}, ... } */
  var canFD = {};      /* 系统接口 CAN FD 支持映射 { can0: 1, ... } */
  var canEnabled = null; /* Confirmed global state; loaded from the device. */
  var canFeatureBusy = false;
  var netLoaded = false;
  var netLoading = false;
  var netSaving = false;
  var netSnapshot = null;

  function renderNetControls() {
    var unavailable = !netLoaded || netLoading || netSaving;
    ['bind', 'ip_mode', 'ip_a', 'ip_m', 'ip_g'].forEach(function(id) {
      document.getElementById(id).disabled = unavailable;
    });
    document.getElementById('network_save').disabled = unavailable || !App.components.get('network-ano') || !App.components.get('network-ano').ready();
  }
  document.addEventListener('app:componentchange', renderNetControls);
  renderNetControls();

  function netSettings() {
    return {
      target: 'ip',
      ip_ifname: document.getElementById('bind').value,
      ip_mode: document.getElementById('ip_mode').value,
      ip_addr: document.getElementById('ip_a').value.trim(),
      ip_mask: document.getElementById('ip_m').value.trim(),
      ip_gw: document.getElementById('ip_g').value.trim()
    };
  }

  function renderCanFeature() {
    var enabled = canEnabled === true;
    var button = document.getElementById('can_feature_toggle');
    button.disabled = canFeatureBusy || canEnabled === null;
    button.setAttribute('aria-pressed', String(enabled));
    setI18nText(button.firstElementChild, enabled ? '停用 CAN' : '启用 CAN');
    document.getElementById('card_can').hidden = !enabled;
    document.querySelector('.can-grid').classList.toggle('can-disabled', !enabled);
    var unavailable = !enabled || canFeatureBusy || canNames.length === 0;
    document.querySelectorAll('#card_can input,#card_can select,#card_can button').forEach(function(control) {
      control.disabled = unavailable;
    });
  }

  window.toggleCanFeature = async function() {
    if (canFeatureBusy || canEnabled === null) return;
    var next = !canEnabled;
    canFeatureBusy = true;
    renderCanFeature();
    try {
      var response = await authFetch('/api/config', {
        method: 'POST',
        body: new URLSearchParams({target: 'can_feature', can_enabled: next ? 'on' : 'off'})
      });
      if (!response.ok) throw new Error('CAN setting rejected');
      canEnabled = next;
      cardStatus('can_feature', 'ok', next ? 'CAN 已启用' : 'CAN 已停用');
    } catch(e) {
      cardStatus('can_feature', 'err', 'CAN 开关保存失败，请重试');
    } finally {
      canFeatureBusy = false;
      renderCanFeature();
    }
  };

  function curName() { return canNames[curCan] || ('can' + curCan); }

  function chkFd() {
    document.getElementById('dbr').style.display = document.getElementById('fd').value === 'on' ? 'flex' : 'none';
  }

  var cardStatus = App.status;

  function updateFileName(input) {
    var label = document.querySelector('[data-file-name="' + input.id + '"]');
    if (!label) return;
    if (input.files && input.files.length) {
      label.removeAttribute('data-i18n');
      label.textContent = input.files[0].name;
      label.title = input.files[0].name;
    } else {
      label.removeAttribute('title');
      setI18nText(label, '未选择文件');
    }
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
    if (!canNames.length) return;
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
    if (!canNames.length) {
      document.getElementById('cfg').style.display = 'none';
      document.getElementById('hint').style.display = 'block';
      setI18nText(document.getElementById('hint'), '系统未检测到 CAN 设备');
    }
    renderCanFeature();
  }

  window.onCanSel = function() {
    collectCan();
    curCan = parseInt(document.getElementById('canSel').value, 10) || 0;
    renderCan();
  };

  window.toggleCan = async function(on) {
    if (!canEnabled || !canNames.length || canFeatureBusy) return;
    var n = curName();
    var tg = document.getElementById('tg');
    var previous = (cansCfg[n] || {}).up === 'on';
    tg.disabled = true;
    try {
      var response = await authFetch('/api/can/toggle', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'ifname=' + encodeURIComponent(n) + '&action=' + (on ? 'up' : 'down')
      });
      if (!response.ok) throw new Error('CAN interface setting rejected');
      cansCfg[n] = cansCfg[n] || {};
      cansCfg[n].up = on ? 'on' : 'off';
      setSwitch(on);
    } catch(e) {
      setSwitch(previous);
      cardStatus('can', 'err', '请求失败');
    } finally { renderCanFeature(); }
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
    sel.innerHTML = '<option value="" data-i18n="选择网卡">' + t('选择网卡') + '</option>';
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
    if (canFeatureBusy) return;
    canFeatureBusy = true;
    renderCanFeature();
    var loadNet = !netSaving;
    var netFailed = loadNet;
    if (loadNet) {
      netLoaded = false;
      netLoading = true;
      renderNetControls();
    }
    try {
      var response = await fetch('/api/config');
      if (!response.ok) throw new Error('Settings unavailable');
      var cfg = await response.json();
      if (!cfg || typeof cfg !== 'object') throw new Error('Invalid settings');
      if (loadNet) {
        /* Web readiness is independent of CAN state and other configuration cards. */
        var bind = document.getElementById('bind');
        var ifname = cfg.ip_ifname || '';
        if (ifname && !Array.from(bind.options).some(function(option) { return option.value === ifname; })) {
          var option = document.createElement('option');
          option.value = ifname;
          option.textContent = ifname;
          bind.appendChild(option);
        }
        bind.value = ifname;
        document.getElementById('ip_mode').value = cfg.ip_mode || '';
        document.getElementById('ip_a').value = cfg.ip_addr || '';
        document.getElementById('ip_m').value = cfg.ip_mask || '';
        document.getElementById('ip_g').value = cfg.ip_gw || '';
        chkIpMode();
        netSnapshot = netSettings();
        netLoaded = true;
        netFailed = false;
      }
      if (typeof cfg.can_enabled !== 'boolean') throw new Error('CAN state unavailable');

      /* YOLO thresholds are percentages; concurrency and pacing are automatic. */
      document.getElementById('yolo_cf').value = Math.round((cfg.yolo_conf || 0.25) * 100);
      document.getElementById('yolo_nm').value = Math.round((cfg.yolo_nms || 0.45) * 100);

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
      canEnabled = cfg.can_enabled;
      renderCan();
    } catch(e) {
      if (netFailed) cardStatus('net', 'err', '配置加载失败，请点击查询重试');
      cardStatus('can_feature', 'err', '配置加载失败，请点击查询重试');
    } finally {
      if (loadNet) netLoading = false;
      renderNetControls();
      canFeatureBusy = false;
      renderCanFeature();
    }
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

  /* 保存 Web 管理界面的网卡 IP 配置，HTTP 端口固定为 80。 */
  window.saveNet = async function() {
    if (!netLoaded || netLoading || netSaving || !App.components.get('network-ano') || !App.components.get('network-ano').validate()) return;
    var ipSettings = netSettings();
    var ipChanged = Object.keys(ipSettings).some(function(key) { return ipSettings[key] !== netSnapshot[key]; });
    var mode = ipSettings.ip_mode;
    var ifn = ipSettings.ip_ifname;
    if (mode !== '') {
      if (!ifn) { cardStatus('net', 'err', '配置 IP 需要先选择绑定网卡'); return; }
      if (mode === 'static') {
        var a = document.getElementById('ip_a').value.trim();
        var m = document.getElementById('ip_m').value.trim();
        if (!a || !m) { cardStatus('net', 'err', '静态模式需要填写 IP 地址与子网掩码'); return; }
        if (ipChanged && !confirm(t('将把 ') + ifn + t(' 设为静态 IP ') + a + '/' + m
          + (document.getElementById('ip_g').value ? t('，网关 ') + document.getElementById('ip_g').value : '')
          + t('。\n若当前页面通过该网卡访问，连接会断开，请用新 IP 重新访问。'))) return;
      } else if (mode === 'dhcp') {
        if (ipChanged && !confirm(t('将把 ') + ifn + t(' 切换到 DHCP 自动获取。\nIP 变化后请用新地址重新访问。'))) return;
      }
    }
    netSaving = true;
    App.components.get('network-ano').setSaving(true);
    renderNetControls();
    try {
      if (!await App.components.get('network-ano').save()) return;
      if (ipChanged) {
        if (await postConfig(ipSettings, 'net')) netSnapshot = ipSettings;
      } else {
        cardStatus('net', 'ok', '✓ 已保存');
      }
    } finally {
      netSaving = false;
      App.components.get('network-ano').setSaving(false);
      renderNetControls();
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
  window.saveYolo = async function() {
    await postConfig({target:'yolo', yolo_conf:document.getElementById('yolo_cf').value,
      yolo_nms:document.getElementById('yolo_nm').value}, 'yolo');
  };

  window.uploadYolo = async function(type) {
    var input = document.getElementById(type === 'model' ? 'yolo_model_f' : 'yolo_names_f');
    var file = input.files && input.files[0];
    if (!file) { cardStatus('yolo', 'err', type === 'model' ? '请先选择模型文件' : '请先选择类别标签文件'); return; }
    try {
      var r = await authFetch('/api/yolo/upload?type=' + type, {
        method: 'POST',
        headers: {'Content-Type': 'application/octet-stream'},
        body: file
      });
      var j = r.ok ? await r.json() : null;
      if (j && j.result === 'ok') {
        cardStatus('yolo', 'ok', type === 'names' ? '✓ 已上传并热生效 ({classes} classes)' : '✓ 已上传并热生效', {classes: j.classes});
        setTimeout(loadCfg, 800);
      } else if (j) {
        cardStatus('yolo', 'err', '上传失败 ({status})', {status: j.result || ('HTTP ' + r.status)});
      } else {
        cardStatus('yolo', 'err', '上传失败 ({status})', {status: r.status});
      }
    } catch(e) { cardStatus('yolo', 'err', '上传失败'); }
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
  loadCanIfaces().then(loadIfaces).then(loadCfg);

})();
