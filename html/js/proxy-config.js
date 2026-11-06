// SPDX-License-Identifier: GPL-3.0-or-later
/* proxy-config.js — board-wide network proxy editor on /config.
   Standalone module: no dependency on the Codex chat bridge. */
(function () {
  'use strict';
  function el(id) { return document.getElementById(id); }
  var card = el('card_proxy');
  if (!card) return;

  var host = el('proxy_host');
  var port = el('proxy_port');
  var scheme = el('proxy_scheme');
  var bypass = el('proxy_no_proxy');
  var pathHint = el('proxy_path_hint');
  var urlText = el('proxy_url');
  var systemText = el('proxy_system');
  var badge = el('proxy_service_badge');
  var status = el('status_proxy');
  var rawPre = el('proxy_raw_pre');
  var pending = false;

  function setStatus(text, kind) {
    if (status) {
      status.textContent = text;
      status.className = 'card-status proxy-status' + (kind ? ' ' + kind : '');
    }
  }

  function preview() {
    if (urlText) {
      urlText.textContent = scheme.value + '://' +
        (host.value.trim() || '—') + ':' + (port.value.trim() || '—');
    }
  }

  function payload(apply) {
    return {
      PROXY_HOST: host.value.trim(),
      PROXY_PORT: port.value.trim(),
      PROXY_SCHEME: scheme.value,
      NO_PROXY: bypass.value.trim(),
      apply: !!apply
    };
  }

  function render(data) {
    if (!data || data.ok === false) {
      setStatus((data && data.error) || t('读取失败'), 'warn');
      return;
    }
    var c = data.config || {};
    if (document.activeElement !== host) host.value = c.PROXY_HOST || '';
    if (document.activeElement !== port) port.value = c.PROXY_PORT || '';
    if (document.activeElement !== scheme) scheme.value = c.PROXY_SCHEME || 'http';
    if (document.activeElement !== bypass) bypass.value = c.NO_PROXY || '';
    preview();
    if (pathHint) {
      pathHint.textContent = t('配置文件') + ': ' + (data.path || '') +
        ' · ' + t('影响整机网络流量（保存并应用后生效）');
    }
    if (systemText) {
      var s = data.system || {};
      systemText.textContent = t('系统环境变量') + ': HTTP_PROXY=' +
        (s.HTTP_PROXY || t('未配置')) + ' · NO_PROXY=' +
        (s.NO_PROXY || t('未配置'));
    }
    if (badge) {
      var act = data.service || 'unknown';
      badge.textContent = t('服务状态') + ': ' + act;
      badge.className = 'proxy-badge ' + (act === 'active' ? 'ok' : 'warn');
    }
    if (rawPre) rawPre.textContent = data.raw || t('配置文件不存在');
    setStatus(data.exists ? '' : t('配置文件不存在'), data.exists ? '' : 'warn');
  }

  async function call(url, opts) {
    var r = await authFetch(url, opts);
    var j = await r.json();
    if (!r.ok || !j.ok) {
      throw new Error(j.error || ('HTTP ' + r.status));
    }
    return j;
  }

  async function load() {
    try {
      render(await call('/api/proxy/config', {}));
    } catch (e) {
      setStatus(t('读取失败') + ': ' + e.message, 'warn');
    }
  }

  async function save(apply) {
    if (pending) return;
    pending = true;
    try {
      var j = await call('/api/proxy/config', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload(apply))
      });
      setStatus(apply
        ? (j.applied ? t('已保存并应用') : t('已保存，但应用失败') + ': ' + (j.apply_error || ''))
        : t('已保存（需保存并应用才能对整机生效）'),
        apply && !j.applied ? 'warn' : 'ok');
      await load();
    } catch (e) {
      setStatus(t('保存失败') + ': ' + e.message, 'warn');
    } finally {
      pending = false;
    }
  }

  async function test() {
    if (pending) return;
    pending = true;
    setStatus(t('检测中') + '…');
    try {
      var j = await call('/api/proxy/test', {method: 'POST'});
      var msg = (j.reachable ? t('代理可用') : t('代理不可用')) +
        ' · ' + (j.tcp ? t('端口可达') : t('端口不可达')) +
        ' · ' + t('HTTP 状态码') + '=' + (j.http_code || 0);
      if (j.tcp_error) msg += ' · ' + j.tcp_error;
      if (j.curl_error) msg += ' · ' + j.curl_error;
      setStatus(msg, j.reachable ? 'ok' : 'warn');
    } catch (e) {
      setStatus(t('检测失败') + ': ' + e.message, 'warn');
    } finally {
      pending = false;
    }
  }

  [host, port, scheme, bypass].forEach(function (node) {
    if (node) node.addEventListener('input', preview);
    if (node) node.addEventListener('change', preview);
  });
  var bSave = el('proxy_save');
  var bApply = el('proxy_apply');
  var bTest = el('proxy_test');
  var bReload = el('proxy_reload');
  if (bSave) bSave.addEventListener('click', function () { save(false); });
  if (bApply) bApply.addEventListener('click', function () { save(true); });
  if (bTest) bTest.addEventListener('click', test);
  if (bReload) bReload.addEventListener('click', load);
  load();
})();
