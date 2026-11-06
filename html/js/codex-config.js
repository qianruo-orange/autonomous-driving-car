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
      /* 只读接口，沿用页面自带的 Basic 凭据，不触发管理员登录框 */
      var data = await App.json('/api/codex/models', {timeout: 45000});
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
    /* 显示剩余可用比例：满条=额度充足，余量少时变黄/红 */
    var left = Math.max(0, Math.min(100, 100 - window.used_percent));
    bar.style.width = left.toFixed(0) + '%';
    bar.className = left <= 15 ? 'bad' : left <= 40 ? 'warn' : '';
    pct.textContent = left.toFixed(0) + '%';
  }

  function clockOf(seconds) {
    if (!seconds) return '';
    var d = new Date(seconds * 1000);
    if (isNaN(d.getTime())) return '';
    return d.toLocaleString(undefined, {month: '2-digit', day: '2-digit',
                                        hour: '2-digit', minute: '2-digit'});
  }

  function windowLabel(minutes) {
    if (!minutes) return '';
    if (minutes <= 360) return t('{hours} 小时', {hours: Math.round(minutes / 60)});
    if (minutes >= 10080) return t('每周');
    return t('{days} 天', {days: Math.round(minutes / 1440)});
  }

  /* 后端按 primary/secondary 返回，但两个窗口的语义随套餐变化；按窗口长度
     分配标签，短窗口永远在左、长窗口在右，避免 5 小时与每周写反。 */
  function splitWindows(data) {
    var a = data.primary, b = data.secondary;
    if (!a) return [null, b];
    if (!b) return [a, null];
    return (a.window_minutes || 0) <= (b.window_minutes || 0) ? [a, b] : [b, a];
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
    var windows = splitWindows(data);
    var short = windows[0], long = windows[1];
    setMeter('codex_bar_5h', 'codex_pct_5h', short);
    setMeter('codex_bar_wk', 'codex_pct_wk', long);
    var label5 = el('codex_label_5h'), labelWk = el('codex_label_wk');
    if (label5) label5.textContent = (short && windowLabel(short.window_minutes)) || t('短期额度');
    if (labelWk) labelWk.textContent = (long && windowLabel(long.window_minutes)) || t('长期额度');
    var parts = [];
    if (!data.allowed) parts.push(t('额度已用尽'));
    var reset = clockOf(long && long.resets_at);
    if (reset) parts.push(t('{label} {time} 重置', {
      label: (long && windowLabel(long.window_minutes)) || t('额度'), time: reset}));
    if (data.credits && data.credits.has && data.credits.balance) {
      parts.push(t('余额 {balance}', {balance: data.credits.balance}));
    }
    if (data.reset_credits) parts.push(t('可用重置次数 {count}', {count: data.reset_credits}));
    quotaReset.textContent = parts.join(' · ');
  }

  async function loadQuota() {
    try {
      /* 只读接口：浏览器自带的 Basic 凭据足够，不走 authFetch（避免每次加载弹
         管理员登录框）。 */
      paintQuota(await App.json('/api/codex/quota', {timeout: 45000}));
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
      /* 板子是 http://192.168.1.7，非安全上下文下没有 navigator.clipboard，
         退回到 textarea + execCommand 这条老路 */
      if (navigator.clipboard && window.isSecureContext) {
        await navigator.clipboard.writeText(code);
      } else {
        var box = document.createElement('textarea');
        box.value = code;
        box.setAttribute('readonly', '');
        box.style.cssText = 'position:fixed;top:0;left:0;opacity:0';
        document.body.appendChild(box);
        box.select();
        box.setSelectionRange(0, box.value.length);
        var copied = document.execCommand('copy');
        box.remove();
        if (!copied) throw new Error('copy blocked');
      }
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
  var progressTimer = null;

  function stopProgress() {
    if (progressTimer) { clearInterval(progressTimer); progressTimer = null; }
  }

  function paintProgress(d) {
    var mb = (d.downloaded || 0) / 1048576;
    var tot = (d.total || 0) / 1048576;
    var txt = t('正在下载更新 {percent}%', {percent: d.percent || 0}) +
              ' · ' + mb.toFixed(1) + ' / ' + Math.round(tot) + ' MB';
    var phase = String(d.phase || '').replace(/^==>\s*/, '');
    if (phase && !/Downloading/i.test(phase)) txt += ' · ' + phase;
    updateResult.hidden = false;
    updateResult.className = 'codex-update-text accent';
    updateResult.textContent = txt;
  }

  function fetchProgress() {
    return App.json('/api/codex/update/status', {timeout: 15000});
  }

  /* 安装/下载期间每 2.5 秒刷新一次进度；结束后回到常规的版本检查 */
  function startProgress() {
    stopProgress();
    installing = true;
    canInstall = false;
    updateBtn.disabled = true;
    updateState = {mode: 'downloading', cls: 'accent'};
    paintUpdate();
    progressTimer = setInterval(async function () {
      var d;
      try {
        d = await fetchProgress();
      } catch (error) {
        return;
      }
      paintProgress(d);
      if (d.running) return;
      stopProgress();
      installing = false;
      updateBtn.disabled = false;
      var tail = String(d.log || '').trim().split('\n').pop().slice(0, 160);
      await loadModels();
      await load();
      await checkUpdate();
      if (updateState.mode === 'ok' && updateState.data.update_available && tail) {
        updateState.data.failTail = tail;
        paintUpdate();
      }
    }, 2500);
  }

  function paintUpdate() {
    updateResult.hidden = !updateState;
    updateBtn.textContent = canInstall ? t('安装更新') : t('检查更新');
    updateBtn.className = 'btn btn-back' + (canInstall ? ' ready' : '');
    updateBtn.disabled = installing;
    if (!updateState) return;
    updateResult.className = 'codex-update-text' + (updateState.cls ? ' ' + updateState.cls : '');
    if (updateState.mode === 'downloading') return;   /* 进度由 paintProgress 维护 */
    if (updateState.mode === 'checking') {
      updateResult.textContent = t('检查中…'); return;
    }
    if (updateState.mode === 'installing') {
      updateResult.textContent = t('正在启动安装…'); return;
    }
    if (updateState.mode === 'error') {
      updateResult.textContent = t('检查更新失败') + ': ' + updateState.error; return;
    }
    if (updateState.mode === 'installed') {
      var r = updateState.data;
      var tail = String(r.output || '').trim().split('\n').pop().slice(0, 160);
      var done = r.ok
        ? t('已更新到 {version}', {version: r.version || '—'})
        : t('安装失败') + (r.error ? ': ' + r.error : (tail ? ': ' + tail : ''));
      updateResult.textContent = done;
      return;
    }
    var d = updateState.data;
    var txt = t('当前 {current} · 最新 {latest}',
      {current: d.current || '—', latest: d.latest || '—'});
    if (!d.cli_found) txt += ' · ' + t('未找到 Codex CLI');
    else if (d.update_available) txt += ' · ' + t('有可用更新，点击安装更新');
    else txt += ' · ' + t('已是最新版本');
    if (d.stale) txt += ' · ' + t('使用上次检查结果');
    if (d.failTail) txt += ' · ' + t('上次尝试失败') + ': ' + d.failTail;
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
        {authenticated: true, method: 'POST', timeout: 60000});
      if (data && data.started) {
        /* 后台单元已经开始下载：立刻切到进度条模式 */
        App.status('codex', 'ok', t('已在后台开始更新'));
        startProgress();
        return;
      }
      updateState = {mode: 'installed', data: data || {},
                     cls: data && data.ok ? 'accent' : 'err'};
      App.status('codex', data && data.ok ? 'ok' : 'err',
        t('已更新到 {version}', {version: (data && data.version) || '—'}));
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

  /* 打开页面时如果后台正在更新，直接显示进度 */
  (async function () {
    try {
      var d = await fetchProgress();
      if (d && d.running) startProgress();
    } catch (error) {}
  })();
})();
