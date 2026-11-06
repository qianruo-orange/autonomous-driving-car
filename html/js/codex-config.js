// SPDX-License-Identifier: GPL-3.0-or-later
/* codex-config.js — Codex CLI login and settings on /config.
   One title-bar button serves both directions: "Sign in" (start / retry the
   device-code flow) when signed out, "Sign out" when signed in. */
(function () {
  'use strict';
  function el(id) { return document.getElementById(id); }
  var loginBtn = el('codex_login_btn');
  if (!loginBtn) return;
  var loginArea = el('codex_login_area');
  var deviceLink = el('codex_device_link');
  var deviceCode = el('codex_device_code');
  var deviceCopy = el('codex_device_copy');
  var deviceHint = el('codex_device_hint');
  var modelSel = el('codex_model');
  var effortSel = el('codex_effort');
  var pending = false;
  var saving = false;
  var loggedIn = false;
  var loginRequested = false;

  function render(data) {
    var login = data.login || {};
    loggedIn = !!data.logged_in;
    var text, cls = '';
    if (login.state === 'starting' || login.state === 'pending') {
      text = t('等待浏览器授权'); cls = 'warn';
    } else if (loggedIn) {
      text = t('退出登录'); cls = 'ok';
    } else {
      /* Signed out, or the last attempt failed — the button starts or
         retries the device-code sign-in. */
      text = t('登录');
    }
    loginBtn.textContent = text;
    loginBtn.className = 'btn btn-back codex-login-btn' + (cls ? ' ' + cls : '');
    loginBtn.disabled = pending ||
      login.state === 'starting' || login.state === 'pending';

    var s = data.settings || {};
    if (document.activeElement !== modelSel) modelSel.value = s.model || '';
    if (document.activeElement !== effortSel) effortSel.value = s.effort || 'medium';

    var url = login.url || '';
    var code = login.code || '';
    loginArea.hidden = loggedIn || !loginRequested;
    deviceLink.textContent = url || t('正在获取登录链接…');
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
      setI18nText(deviceHint, '正在获取一次性设备码，拿到后打开链接输入即可完成授权。');
    }
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

  async function saveSettings() {
    if (saving) return;
    saving = true;
    try {
      var response = await authFetch('/api/codex/config', {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({model: modelSel.value, effort: effortSel.value})
      });
      var result = await response.json();
      if (!result.ok) throw new Error(result.error || 'HTTP ' + response.status);
      App.status('codex', 'ok', t('已保存 Codex 设置'));
      await load();
    } catch (error) {
      App.status('codex', 'err', t('Codex 设置保存失败') + ': ' + error.message);
    } finally {
      saving = false;
    }
  }
  modelSel.addEventListener('change', saveSettings);
  effortSel.addEventListener('change', saveSettings);

  loginBtn.addEventListener('click', async function () {
    if (loginBtn.disabled) return;
    if (loggedIn) {
      if (!confirm(t('确定退出 Codex 登录并清除已保存的凭据？'))) return;
      loginBtn.disabled = true;
      try {
        var out = await authFetch('/api/codex/logout', {method: 'POST'});
        if (!out.ok) throw new Error('HTTP ' + out.status);
        App.status('codex', 'ok', t('已退出登录'));
        loginRequested = false;
      } catch (error) {
        App.status('codex', 'err', t('退出登录失败'));
      }
      await load();
      return;
    }
    pending = true; loginBtn.disabled = true;
    loginRequested = true;
    loginArea.hidden = false;
    try {
      var response = await authFetch('/api/codex/login/device', {method: 'POST'});
      var result = await response.json();
      if (!result.ok) throw new Error(result.error || 'HTTP ' + response.status);
      App.status('codex', 'ok', t('已发起设备码登录'));
      await load();
    } catch (error) {
      App.status('codex', 'err', t('发起登录失败') + ': ' + error.message);
    } finally { pending = false; await load(); }
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

  var updateBtn = el('codex_update_check');
  var updateResult = el('codex_update_result');
  var updateState = null;

  function paintUpdate() {
    updateResult.hidden = !updateState;
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
})();
