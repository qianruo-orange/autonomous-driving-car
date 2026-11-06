// SPDX-License-Identifier: GPL-3.0-or-later
/* proxy-config.js — board-wide network proxy editor on /config.
   Standalone module: no dependency on the Codex chat bridge. */
(function () {
  'use strict';
  function el(id) { return document.getElementById(id); }
  var card = el('card_network');
  if (!card) return;

  var host = el('proxy_host');
  var port = el('proxy_port');
  var scheme = el('proxy_scheme');
  var bypass = el('proxy_no_proxy');
  var switchBox = el('proxy_enabled');
  var badge = el('proxy_service_badge');
  var status = el('status_proxy');
  var pending = false;

  function setStatus(text, kind) {
    if (status) {
      status.textContent = text;
      status.className = 'card-status proxy-status' + (kind ? ' ' + kind : '');
    }
  }

  function payload(apply) {
    return {
      PROXY_HOST: host.value.trim(),
      PROXY_PORT: port.value.trim(),
      PROXY_SCHEME: scheme.value,
      NO_PROXY: bypass.value.trim(),
      enabled: switchBox ? switchBox.checked : true,
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
    if (switchBox) switchBox.checked = !!data.enabled;
    if (badge) {
      var act = data.service || 'unknown';
      badge.textContent = t('服务状态') + ': ' + act;
      badge.className = 'proxy-badge ' + (act === 'active' ? 'ok' : 'warn');
    }
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

  async function save() {
    if (pending) return;
    pending = true;
    try {
      var j = await call('/api/proxy/config', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload(true))
      });
      setStatus(j.enabled === false
        ? t('代理已关闭，流量直连')
        : (j.applied ? t('已保存并应用')
                     : t('已保存，但应用失败') + ': ' + (j.apply_error || '')),
        j.enabled === false || j.applied ? 'ok' : 'warn');
      await load();
    } catch (e) {
      setStatus(t('保存失败') + ': ' + e.message, 'warn');
    } finally {
      pending = false;
    }
  }

  var bSave = el('proxy_save');
  if (bSave) bSave.addEventListener('click', save);
  if (switchBox) switchBox.addEventListener('change', function () { save(); });
  load();
})();
