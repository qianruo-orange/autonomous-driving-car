// SPDX-License-Identifier: GPL-3.0-or-later
/* codex-config.js — Codex CLI login and settings on /config.
   One title-bar button serves both directions: "Sign in" (start / retry the
   device-code flow) when signed out, "Sign out" when signed in.
   The model list, the reasoning levels per model and the account quota all
   come from the CLI itself, so the card never shows a model the CLI cannot
   run. */
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
  var models = [];
  var modelsLoaded = false;
  var currentModel = '';
  var currentEffort = 'medium';

  var EFFORT_TEXT = {
    minimal: '极低', low: '低', medium: '中', high: '高',
    xhigh: '极高', max: '最大', ultra: '极限'
  };

  function effortLabel(effort, isDefault) {
    var name = EFFORT_TEXT[effort] ? t(EFFORT_TEXT[effort]) : '';
    return (name ? name + '（' + effort + '）' : effort) + (isDefault ? ' · ' + t('推荐') : '');
  }

  function modelOf(slug) {
    for (var i = 0; i < models.length; i++) {
      if (models[i].slug === slug) return models[i];
    }
    return null;
  }

  function paintEfforts(slug, keep) {
    var model = modelOf(slug);
    var efforts = (model && model.efforts.length) ? model.efforts : ['low', 'medium', 'high'];
    var wanted = keep && efforts.indexOf(keep) >= 0 ? keep : (model && model.default_effort) || 'medium';
    if (efforts.indexOf(wanted) < 0) wanted = efforts[0];
    effortSel.replaceChildren();
    efforts.forEach(function (effort) {
      var option = document.createElement('option');
      option.value = effort;
      option.textContent = effortLabel(effort, model && model.default_effort === effort);
      if (effort === wanted) option.selected = true;
      effortSel.appendChild(option);
    });
    currentEffort = wanted;
  }

  function paintModels(selected) {
    modelSel.replaceChildren();
    var found = false;
    var fallback = document.createElement('option');
    fallback.value = '';
    setI18nText(fallback, '默认（跟随 CLI）');
    if (!selected) { fallback.selected = true; found = true; }
    modelSel.appendChild(fallback);
    models.forEach(function (model) {
      var option = document.createElement('option');
      option.value = model.slug;
      option.textContent = model.name && model.name !== model.slug
        ? model.name + ' (' + model.slug + ')' : model.slug;
      if (model.slug === selected) { option.selected = true; found = true; }
      modelSel.appendChild(option);
    });
    if (selected && !found) {
      /* A model saved before a CLI downgrade stays selectable, but is marked. */
      var kept = document.createElement('option');
      kept.value = selected;
      kept.textContent = selected + ' · ' + t('当前已保存');
      kept.selected = true;
      modelSel.appendChild(kept);
    }
    currentModel = selected || '';
    paintEfforts(currentModel, currentEffort);
  }

  async function loadModels() {
    try {
      var data = await App.json('/api/codex/models', {authenticated: true, timeout: 45000});
      if (!data.ok) throw new Error(data.error || 'HTTP 500');
      models = data.models || [];
      modelsLoaded = true;
      paintModels(currentModel);
    } catch (error) {
      modelsLoaded = false;
      if (!modelSel.options.length) {
        var option = document.createElement('option');
        option.value = '';
        setI18nText(option, '模型列表读取失败');
        modelSel.appendChild(option);
      }
    }
  }

  /* ---- 额度 ---- */
  var quotaBox = el('codex_quota');
  var quotaPlan = el('codex_plan');
  var quotaReset = el('codex_reset');
  var quotaStop = null;

  function setMeter(barId, pctId, window) {
    var bar = el(barId), pct = el(pctId);
    if (!bar || !pct) return;
    if (!window || typeof window.used_percent !== 'number') {
      bar.style.width = '0%'; bar.className = '';
      pct.textContent = '—';
      return;
    }
    var used = Math.max(0, Math.min(100, window.used_percent));
    bar.style.width = used.toFixed(0) + '%';
    bar.className = used >= 85 ? 'bad' : used >= 60 ? 'warn' : '';
    pct.textContent = used.toFixed(0) + '%';
  }

  function clockOf(seconds) {
    if (!seconds) return '';
    var d = new Date(seconds * 1000);
    if (isNaN(d.getTime())) return '';
    return d.toLocaleString(undefined, {month: '2-digit', day: '2-digit',
                                        hour: '2-digit', minute: '2-digit'});
  }

  function paintQuota(data) {
    if (!quotaBox) return;
    if (!data || data.ok === false || !data.signed_in) {
      quotaBox.hidden = true;
      return;
    }
    quotaBox.hidden = false;
    var plan = String(data.plan || '').toLowerCase();
    quotaPlan.textContent = plan ? plan.toUpperCase() : '—';
    quotaPlan.className = 'codex-plan' + (data.allowed ? '' : ' warn');
    setMeter('codex_bar_5h', 'codex_pct_5h', data.primary);
    setMeter('codex_bar_wk', 'codex_pct_wk', data.secondary);
    var parts = [];
    if (!data.allowed) parts.push(t('额度已用尽'));
    var reset = clockOf(data.secondary && data.secondary.resets_at);
    if (reset) parts.push(t('每周额度 {time} 重置', {time: reset}));
    if (data.credits && data.credits.has && data.credits.balance) {
      parts.push(t('余额 {balance}', {balance: data.credits.balance}));
    }
    if (data.reset_credits) parts.push(t('可用重置次数 {count}', {count: data.reset_credits}));
    quotaReset.textContent = parts.join(' · ');
  }

  async function loadQuota() {
    try {
      paintQuota(await App.json('/api/codex/quota', {authenticated: true, timeout: 30000}));
    } catch (error) {
      if (quotaBox) quotaBox.hidden = true;
    }
  }

  function syncQuota() {
    if (loggedIn && !quotaStop) {
      quotaStop = App.poll(loadQuota, {interval: 60000});
    } else if (!loggedIn && quotaStop) {
      quotaStop();
      quotaStop = null;
      if (quotaBox) quotaBox.hidden = true;
    }
  }

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
    var savedModel = s.model || '';
    var savedEffort = s.effort || 'medium';
    if (!modelsLoaded) {
      if (!modelSel.options.length) {
        /* 目录还没回来时先占位，避免选中态被空列表清掉 */
        var placeholder = document.createElement('option');
        placeholder.value = savedModel;
        placeholder.textContent = savedModel || t('默认（跟随 CLI）');
        modelSel.appendChild(placeholder);
        currentModel = savedModel;
      }
    } else {
      /* 服务端保存的模型变了（别处改过或刚登录），按目录重建一次 */
      if (savedModel !== currentModel) paintModels(savedModel);
      if (document.activeElement !== modelSel) modelSel.value = currentModel;
      if (document.activeElement !== effortSel) {
        var model = modelOf(currentModel);
        if (!model || model.efforts.indexOf(savedEffort) >= 0) {
          effortSel.value = savedEffort;
          currentEffort = savedEffort;
        }
      }
    }

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
    syncQuota();
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
  loadModels();
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
      currentModel = modelSel.value;
      currentEffort = effortSel.value;
      App.status('codex', 'ok', t('已保存 Codex 设置'));
      await load();
    } catch (error) {
      App.status('codex', 'err', t('Codex 设置保存失败') + ': ' + error.message);
    } finally {
      saving = false;
    }
  }
  modelSel.addEventListener('change', function () {
    paintEfforts(modelSel.value, '');
    saveSettings();
  });
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
  var canInstall = false;
  var installing = false;

  function paintUpdate() {
    updateResult.hidden = !updateState;
    updateBtn.textContent = canInstall ? t('安装更新') : t('检查更新');
    updateBtn.className = 'btn btn-back' + (canInstall ? ' ready' : '');
    updateBtn.disabled = installing;
    if (!updateState) return;
    updateResult.className = 'codex-update-text' + (updateState.cls ? ' ' + updateState.cls : '');
    if (updateState.mode === 'checking') {
      updateResult.textContent = t('检查中…'); return;
    }
    if (updateState.mode === 'installing') {
      updateResult.textContent = t('正在安装更新，请勿断电…'); return;
    }
    if (updateState.mode === 'error') {
      updateResult.textContent = t('检查更新失败') + ': ' + updateState.error; return;
    }
    if (updateState.mode === 'installed') {
      var r = updateState.data;
      var done = r.ok
        ? t('已更新到 {version}', {version: r.version || '—'})
        : t('安装失败') + (r.error ? ': ' + r.error : '');
      updateResult.textContent = done + (r.ok ? '' : ' · ' + t('可查看板端日志重试'));
      return;
    }
    var d = updateState.data;
    var txt = t('当前 {current} · 最新 {latest}',
      {current: d.current || '—', latest: d.latest || '—'});
    if (!d.cli_found) txt += ' · ' + t('未找到 Codex CLI');
    else if (d.update_available) txt += ' · ' + t('有可用更新，点击安装更新');
    else txt += ' · ' + t('已是最新版本');
    if (d.stale) txt += ' · ' + t('使用上次检查结果');
    updateResult.textContent = txt;
  }

  async function checkUpdate() {
    canInstall = false;
    updateState = {mode: 'checking', cls: ''}; paintUpdate();
    updateBtn.disabled = true;
    try {
      var data = await App.json('/api/codex/update/check',
        {authenticated: true, timeout: 30000});
      if (!data.ok) throw new Error(data.error || 'HTTP 500');
      canInstall = !!data.update_available;
      updateState = {mode: 'ok', data: data, cls: canInstall ? 'accent' : ''};
    } catch (error) {
      canInstall = false;
      updateState = {mode: 'error', error: error.message, cls: 'err'};
    }
    paintUpdate();
  }

  async function installUpdate() {
    installing = true;
    canInstall = false;
    updateState = {mode: 'installing', cls: 'accent'}; paintUpdate();
    try {
      var data = await App.json('/api/codex/update/install',
        {authenticated: true, method: 'POST', timeout: 600000});
      updateState = {mode: 'installed', data: data || {}, cls: data && data.ok ? 'accent' : 'err'};
      App.status('codex', data && data.ok ? 'ok' : 'err',
        data && data.ok ? t('已更新到 {version}', {version: data.version || '—'}) : t('安装失败'));
      await loadModels();
      await load();
    } catch (error) {
      updateState = {mode: 'installed', data: {ok: false, error: error.message}, cls: 'err'};
    } finally {
      installing = false;
      canInstall = false;
      paintUpdate();
    }
  }

  updateBtn.addEventListener('click', function () {
    if (installing) return;
    if (canInstall) installUpdate(); else checkUpdate();
  });
})();
