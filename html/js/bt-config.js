// SPDX-License-Identifier: GPL-3.0-or-later
/* bt-config.js — 配置页「蓝牙连接」卡片：适配器开关、扫描、下拉选择设备后配对/连接/断开/忘记。 */
(function () {
  'use strict';

  var card = document.getElementById('card_bluetooth');
  if (!card) return;

  var powerBox = document.getElementById('bt_powered');
  var discBox = document.getElementById('bt_discoverable');
  var scanBtn = document.getElementById('bt_scan');
  var metaEl = document.getElementById('bt_meta');
  var selEl = document.getElementById('bt_device_sel');
  var detailEl = document.getElementById('bt_detail');
  var statusEl = document.getElementById('status_bt');
  var connBtn = document.getElementById('bt_conn');
  var pairBtn = document.getElementById('bt_pairforget');

  var timer = null;
  var busy = false;
  var scanning = false;
  var devices = [];
  var selected = '';
  var lastSig = '';

  function setStatus(text, kind) {
    if (!statusEl) return;
    statusEl.textContent = text || '';
    statusEl.className = 'card-status' + (kind ? ' ' + kind : '');
  }

  function el(tag, cls, text) {
    var n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }

  function deviceLabel(d) {
    var name = d.alias || d.name || d.address;
    var extra = [];
    if (d.connected) extra.push(t('已连接'));
    else if (d.paired) extra.push(t('已配对'));
    if (d.has_rssi) extra.push(d.rssi + ' dBm');
    return extra.length ? (name + ' · ' + extra.join(' · ')) : name;
  }

  function current() {
    for (var i = 0; i < devices.length; i++) if (devices[i].address === selected) return devices[i];
    return null;
  }

  function renderMeta(st) {
    metaEl.innerHTML = '';
    if (!st || !st.available) {
      metaEl.appendChild(el('span', 'bt-hint', t('未检测到蓝牙适配器')));
      return;
    }
    metaEl.appendChild(el('span', 'bt-adapter-name', st.alias || st.name || t('未知设备名')));
    metaEl.appendChild(el('span', 'bt-addr', st.address || ''));
    metaEl.appendChild(el('span', 'bt-badge ' + (st.powered ? 'ok' : 'off'),
      st.powered ? t('已开启') : t('已关闭')));
    if (st.discovering) metaEl.appendChild(el('span', 'bt-badge live', t('扫描中…')));
  }

  function renderDetail(st) {
    var d = current();
    detailEl.innerHTML = '';
    if (!d) {
      detailEl.appendChild(el('span', 'bt-hint', t('请选择要操作的设备')));
      return;
    }
    detailEl.appendChild(el('span', 'bt-addr', d.address));
    if (d.connected) detailEl.appendChild(el('span', 'bt-badge ok', t('已连接')));
    else if (d.paired) detailEl.appendChild(el('span', 'bt-badge', t('已配对')));
    else detailEl.appendChild(el('span', 'bt-badge', t('未配对')));
    if (d.trusted) detailEl.appendChild(el('span', 'bt-badge', t('已信任')));
    if (d.has_rssi) detailEl.appendChild(el('span', 'bt-rssi', d.rssi + ' dBm'));
  }

  /* 两个按钮按选中设备的状态切换语义：连接/断开、配对/忘记 */
  function renderButtons(st) {
    var d = current();
    var avail = !!(st && st.available && st.powered && d) && !busy;
    if (connBtn) {
      connBtn.textContent = (d && d.connected) ? t('断开') : t('连接');
      connBtn.disabled = !avail;
    }
    if (pairBtn) {
      var paired = !!(d && (d.paired || d.trusted));
      pairBtn.textContent = paired ? t('忘记') : t('配对');
      pairBtn.className = 'btn btn-back' + (paired ? ' bt-danger' : '');
      pairBtn.disabled = !avail;
    }
  }

  function renderSelect(st) {
    var prev = selected;
    selEl.innerHTML = '';

    if (!st || !st.available) {
      selEl.appendChild(new Option(t('未检测到蓝牙适配器'), ''));
      selEl.disabled = true;
      selected = '';
      return;
    }
    if (!devices.length) {
      selEl.appendChild(new Option(scanning ? t('正在搜索附近设备…') : t('暂无设备，点击「扫描」搜索'), ''));
      selEl.disabled = true;
      selected = '';
      return;
    }
    devices.forEach(function (d) { selEl.appendChild(new Option(deviceLabel(d), d.address)); });
    selEl.disabled = false;
    var keep = devices.some(function (d) { return d.address === prev; });
    selected = keep ? prev : devices[0].address;
    selEl.value = selected;
  }

  function applyState(st) {
    var avail = !!(st && st.available);
    if (powerBox) { powerBox.checked = !!(st && st.powered); powerBox.disabled = !avail || busy; }
    if (discBox) { discBox.checked = !!(st && st.discoverable); discBox.disabled = !avail || busy; }
    if (scanBtn) {
      scanning = !!(st && st.discovering);
      scanBtn.textContent = scanning ? t('停止扫描') : t('扫描');
      scanBtn.disabled = !avail || !st.powered || busy;
    }
  }

  function signature(st) {
    return JSON.stringify([st && st.available, st && st.powered, st && st.discoverable,
      st && st.discovering, devices.map(function (d) {
        return [d.address, d.alias, d.name, d.paired, d.connected, d.trusted, d.rssi];
      })]);
  }

  async function refresh(force) {
    if (busy && !force) return;
    try {
      var rs = await Promise.all([
        fetch('/api/bluetooth/status').then(function (r) { return r.json(); }),
        fetch('/api/bluetooth/devices').then(function (r) { return r.json(); })
      ]);
      var st = rs[0], list = rs[1];
      devices = (list && list.devices) || [];
      var sig = signature(st);
      applyState(st);
      if (force || sig !== lastSig) {
        lastSig = sig;
        renderMeta(st);
        renderSelect(st);
        renderDetail(st);
      }
      renderButtons(st);
    } catch (e) {
      setStatus(t('读取失败') + ': ' + e.message, 'warn');
    }
  }

  async function post(url, body) {
    var r = await authFetch(url, {
      method: 'POST',
      headers: {'Content-Type': 'application/x-www-form-urlencoded'},
      body: new URLSearchParams(body).toString()
    });
    var j = await r.json().catch(function () { return {}; });
    if (!r.ok || j.ok === false) throw new Error(j.error || ('HTTP ' + r.status));
    return j;
  }

  function actionLabel(action) {
    switch (action) {
      case 'pair': return t('配对成功');
      case 'connect': return t('已连接');
      case 'disconnect': return t('已断开');
      case 'remove': return t('已忘记该设备');
      case 'trust': return t('已信任该设备');
      default: return t('已完成');
    }
  }

  async function act(address, action) {
    if (!address || busy) return;
    busy = true;
    setStatus(t('正在执行…'), '');
    renderButtons({available: true, powered: true});
    try {
      await post('/api/bluetooth/device', {address: address, action: action});
      if (action === 'pair' || action === 'connect' || action === 'disconnect') {
        setStatus(t('命令已下发，正在等待结果…'), '');
        // 长动作在 worker 里跑：轮询 last，结果由后端按 BlueZ 实际状态判定
        for (var i = 0; i < 40; i++) {
          await new Promise(function (res) { setTimeout(res, 1000); });
          var st = await fetch('/api/bluetooth/status').then(function (r) { return r.json(); });
          if (st && st.last && st.last.address === address && st.last.action === action) {
            setStatus(st.last.ok ? ('✓ ' + actionLabel(action))
                                : (actionLabel(action) + ' ' + t('失败') + ': ' + st.last.error),
                      st.last.ok ? 'ok' : 'err');
            break;
          }
        }
      } else {
        setStatus('✓ ' + actionLabel(action), 'ok');
      }
    } catch (e) {
      setStatus(t('操作失败') + ': ' + e.message, 'err');
    } finally {
      busy = false;
      await refresh(true);
    }
  }

  if (selEl) selEl.addEventListener('change', function () {
    selected = selEl.value;
    renderDetail({available: true, powered: !!(powerBox && powerBox.checked)});
    renderButtons({available: true, powered: !!(powerBox && powerBox.checked)});
  });

  if (connBtn) connBtn.addEventListener('click', function () {
    var d = current();
    if (d) act(d.address, d.connected ? 'disconnect' : 'connect');
  });
  if (pairBtn) pairBtn.addEventListener('click', function () {
    var d = current();
    if (d) act(d.address, (d.paired || d.trusted) ? 'remove' : 'pair');
  });

  if (powerBox) powerBox.addEventListener('change', async function () {
    busy = true;
    try {
      await post('/api/bluetooth/adapter', {powered: powerBox.checked ? 'on' : 'off'});
      setStatus(powerBox.checked ? t('蓝牙已开启') : t('蓝牙已关闭'), 'ok');
    } catch (e) {
      setStatus(t('操作失败') + ': ' + e.message, 'err');
      powerBox.checked = !powerBox.checked;
    } finally { busy = false; await refresh(true); }
  });

  if (discBox) discBox.addEventListener('change', async function () {
    busy = true;
    try {
      await post('/api/bluetooth/adapter', {discoverable: discBox.checked ? 'on' : 'off'});
      setStatus(discBox.checked ? t('已设为可被发现') : t('已取消可被发现'), 'ok');
    } catch (e) {
      setStatus(t('操作失败') + ': ' + e.message, 'err');
      discBox.checked = !discBox.checked;
    } finally { busy = false; await refresh(true); }
  });

  if (scanBtn) scanBtn.addEventListener('click', async function () {
    var next = !scanning;
    busy = true;
    scanBtn.disabled = true;
    try {
      await post('/api/bluetooth/scan', {on: next ? 'on' : 'off'});
      setStatus(next ? t('正在搜索附近设备…') : t('已停止扫描'), '');
      scanning = next;
    } catch (e) {
      setStatus(t('操作失败') + ': ' + e.message, 'err');
    } finally { busy = false; await refresh(true); }
  });

  document.addEventListener('visibilitychange', function () {
    if (document.hidden) {
      if (timer) { clearInterval(timer); timer = null; }
    } else if (!timer) {
      refresh(true);
      timer = setInterval(refresh, 2500);
    }
  });

  refresh(true);
  timer = setInterval(refresh, 2500);
})();
