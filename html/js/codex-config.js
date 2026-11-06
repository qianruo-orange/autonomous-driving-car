// SPDX-License-Identifier: GPL-3.0-or-later
/* codex-config.js — Codex CLI login and settings on /config. */
(function () {
  'use strict';
  function el(id) { return document.getElementById(id); }
  var badge = el('codex_status_badge');
  if (!badge) return;
  var deviceStart = el('codex_device_start');
  var deviceLink = el('codex_device_link');
  var deviceCode = el('codex_device_code');
  var deviceCopy = el('codex_device_copy');
  var deviceHint = el('codex_device_hint');
  var model = el('codex_model');
  var sandbox = el('codex_sandbox');
  var workdir = el('codex_workdir');
  var cliInfo = el('codex_cli_info');
  var pending = false;

  function settingsPayload() {
    return {
      model: model.value.trim(),
      sandbox: sandbox.value,
      workdir: workdir.value.trim()
    };
  }

  function render(data) {
    var login = data.login || {};
    var text, cls = '';
    if (login.state === 'starting' || login.state === 'pending') {
      text = t('等待浏览器授权'); cls = 'warn';
    } else if (data.logged_in) {
      text = t('已登录'); cls = 'ok';
    } else if (login.state === 'error') {
      text = t('登录失败'); cls = 'warn';
    } else {
      text = t('未登录');
    }
    badge.textContent = text;
    badge.className = 'codex-badge ' + cls;

    var s = data.settings || {};
    if (document.activeElement !== model) model.value = s.model || '';
    if (document.activeElement !== sandbox) sandbox.value = s.sandbox || 'read-only';
    if (document.activeElement !== workdir) workdir.value = s.workdir || '/opt/autonomous-driving-car';

    cliInfo.textContent = (data.cli_found ? 'Codex CLI ' + (data.cli_version || '') : t('未找到 Codex CLI')) +
      ' · ' + (data.cli_path || '');

    var url = login.url || '';
    var code = login.code || '';
    deviceLink.textContent = url || t('请先点击开始登录');
    deviceLink.href = url || '#';
    deviceCode.textContent = code || '— — — —';
    deviceCopy.disabled = !code;
    if (login.state === 'pending' || login.state === 'starting') {
      setI18nText(deviceHint, '打开登录链接，输入下面的一次性验证码完成授权（15 分钟内有效）。');
    } else if (login.state === 'success') {
      setI18nText(deviceHint, '登录完成，可以回到主界面开始对话。');
    } else if (login.state === 'error') {
      deviceHint.textContent = login.message || t('设备码登录失败，请重试');
    } else {
      setI18nText(deviceHint, '点击开始设备码登录，然后打开链接并输入验证码完成授权。');
    }
    deviceStart.disabled = pending || login.state === 'starting' || login.state === 'pending';
  }

  async function load(signal) {
    if (document.hidden) return;
    /* Read-only status: reuse the page's Basic Auth credential instead of
       authFetch(), which would open the admin login dialog on page load. */
    var data = await App.json('/api/codex/status',
      {timeout: 15000, signal: signal});
    render(data);
  }

  App.poll(load, {interval: 2000});
  document.addEventListener('languagechange', function () { load(); paintUpdate(); });

  deviceStart.addEventListener('click', async function () {
    pending = true; deviceStart.disabled = true;
    try {
      var response = await authFetch('/api/codex/login/device', {method: 'POST'});
      var result = await response.json();
      if (!result.ok) throw new Error(result.error || 'HTTP ' + response.status);
      App.status('codex', 'ok', t('已发起设备码登录'));
      await load();
    } catch (error) {
      App.status('codex', 'err', t('发起登录失败') + ': ' + error.message);
    } finally { pending = false; deviceStart.disabled = false; }
  });

  deviceCopy.addEventListener('click', async function () {
    var code = deviceCode.textContent.trim();
    if (!code || code.indexOf('—') >= 0) return;
    try {
      await navigator.clipboard.writeText(code);
      App.status('codex', 'ok', t('验证码已复制'));
    } catch (error) {
      App.status('codex', 'err', t('复制失败'));
    }
  });

  el('codex_save_cfg').addEventListener('click', async function () {
    try {
      var response = await authFetch('/api/codex/config', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(settingsPayload())
      });
      var result = await response.json();
      if (!result.ok) throw new Error(result.error || 'HTTP ' + response.status);
      App.status('codex', 'ok', t('设置已保存'));
      await load();
    } catch (error) {
      App.status('codex', 'err', t('设置保存失败') + ': ' + error.message);
    }
  });

  var updateBtn = el('codex_update_check');
  var updateResult = el('codex_update_result');
  var updateState = null;

  function paintUpdate() {
    if (!updateState) return;
    updateResult.className = 'codex-update-text' + (updateState.cls ? ' ' + updateState.cls : '');
    if (updateState.mode === 'checking') {
      updateResult.textContent = t('检查中…'); return;
    }
    if (updateState.mode === 'error') {
      updateResult.textContent = t('检查更新失败') + ': ' + updateState.error; return;
    }
    var d = updateState.data;
    var txt = t('当前 {current} · 最新 {latest}',
      {current: d.current || '—', latest: d.latest || '—'});
    if (!d.cli_found) txt += ' · ' + t('未找到 Codex CLI');
    else if (d.update_available) txt += ' · ' + t('有可用更新') + ' · ' + t('在板端执行 codex update 升级');
    else txt += ' · ' + t('已是最新版本');
    if (d.stale) txt += ' · ' + t('使用上次检查结果');
    updateResult.textContent = txt;
  }

  updateBtn.addEventListener('click', async function () {
    updateBtn.disabled = true;
    updateState = {mode: 'checking', cls: ''}; paintUpdate();
    try {
      var data = await App.json('/api/codex/update/check',
        {authenticated: true, timeout: 30000});
      if (!data.ok) throw new Error(data.error || 'HTTP 500');
      updateState = {mode: 'ok', data: data,
        cls: data.update_available ? 'accent' : ''};
    } catch (error) {
      updateState = {mode: 'error', error: error.message, cls: 'err'};
    }
    paintUpdate();
    updateBtn.disabled = false;
  });

  el('codex_logout').addEventListener('click', async function () {
    if (!confirm(t('确定退出 Codex 登录并清除已保存的凭据？'))) return;
    try {
      var response = await authFetch('/api/codex/logout', {method: 'POST'});
      if (!response.ok) throw new Error('HTTP ' + response.status);
      App.status('codex', 'ok', t('已退出登录'));
      await load();
    } catch (error) {
      App.status('codex', 'err', t('退出登录失败'));
    }
  });
})();
