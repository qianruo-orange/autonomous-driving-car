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
  var apiKey = el('codex_api_key');
  var baseUrl = el('codex_base_url');
  var wireApi = el('codex_wire_api');
  var model = el('codex_model');
  var sandbox = el('codex_sandbox');
  var workdir = el('codex_workdir');
  var cliInfo = el('codex_cli_info');
  var pending = false;

  function settingsPayload() {
    return {
      api_base_url: baseUrl.value.trim(),
      wire_api: wireApi.value,
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
      text = t('已登录') + ' (' + (data.auth_mode === 'apikey' ? 'API Key' : 'ChatGPT') + ')';
      cls = 'ok';
    } else if (login.state === 'error') {
      text = t('登录失败'); cls = 'warn';
    } else {
      text = t('未登录');
    }
    badge.textContent = text;
    badge.className = 'codex-badge ' + cls;

    var s = data.settings || {};
    if (document.activeElement !== baseUrl) baseUrl.value = s.api_base_url || '';
    if (document.activeElement !== wireApi) wireApi.value = s.wire_api || 'responses';
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
      deviceHint.textContent = login.message || t('设备码登录失败，可改用 API Key 登录');
    } else {
      setI18nText(deviceHint, '也可直接使用 API Key 登录；登录链接与验证码由设备码流程生成。');
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
  document.addEventListener('languagechange', function () { load(); });

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

  el('codex_save_key').addEventListener('click', async function () {
    var key = apiKey.value.trim();
    if (!key) { App.status('codex', 'err', t('请输入 API 密钥')); return; }
    var payload = settingsPayload();
    payload.api_key = key;
    try {
      var response = await authFetch('/api/codex/login/apikey', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload)
      });
      var result = await response.json();
      if (!result.ok) throw new Error(result.error || 'HTTP ' + response.status);
      apiKey.value = '';
      App.status('codex', 'ok', t('API 密钥已保存'));
      await load();
    } catch (error) {
      App.status('codex', 'err', t('API 密钥保存失败') + ': ' + error.message);
    }
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
