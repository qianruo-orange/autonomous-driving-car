// SPDX-License-Identifier: GPL-3.0-or-later
/* codex-chat.js — chat card on the main dashboard.
   Polls /api/codex/status, renders the shared conversation log and never
   keeps a model request inside an HTTP worker (the bridge detaches it). */
(function () {
  'use strict';
  var log = document.getElementById('codex_chat_log');
  var form = document.getElementById('codex_chat_form');
  var input = document.getElementById('codex_chat_input');
  var send = document.getElementById('codex_send_btn');
  var stopBtn = document.getElementById('codex_stop_btn');
  var badge = document.getElementById('codex_login_badge');
  var meta = document.getElementById('codex_chat_meta');
  if (!log || !form) return;

  var signature = '';
  var lastData = null;
  var sending = false;

  function shortTime(ts) {
    var m = /T(\d{2}:\d{2}:\d{2})/.exec(ts || '');
    return m ? m[1] : '';
  }

  function bubble(row) {
    var wrap = document.createElement('div');
    var isUser = row.role === 'user';
    wrap.className = 'codex-msg ' + (isUser ? 'user' : 'assistant') +
      (!isUser && row.error ? ' error' : '');
    var box = document.createElement('div');
    box.className = 'codex-bubble';
    box.textContent = String(row.content || '');
    var stamp = document.createElement('span');
    stamp.className = 'codex-meta';
    var bits = [shortTime(row.ts)];
    if (!isUser && row.elapsed_ms) bits.push(Math.round(row.elapsed_ms / 1000) + 's');
    if (row.error) bits.push(t('出错'));
    stamp.textContent = bits.filter(Boolean).join(' · ');
    box.appendChild(stamp);
    wrap.appendChild(box);
    return wrap;
  }

  function render(data) {
    var rows = (data.messages || []).filter(function (r) {
      return r.role === 'user' || r.role === 'assistant';
    });
    var sig = [rows.length, data.busy, data.job, data.last_error,
               data.logged_in, data.login && data.login.state,
               window.getLang()].join('|');
    if (sig === signature) return;
    signature = sig;
    log.replaceChildren();
    if (!rows.length && !data.busy) {
      var empty = document.createElement('div');
      empty.className = 'codex-empty';
      setI18nText(empty, '还没有对话。输入一条消息开始和 Codex 交流。');
      log.appendChild(empty);
    }
    rows.forEach(function (row) {
      log.appendChild(bubble(row));
    });
    if (data.busy) {
      var thinking = document.createElement('div');
      thinking.className = 'codex-msg assistant';
      var box = document.createElement('div');
      box.className = 'codex-bubble codex-typing';
      box.setAttribute('aria-label', t('Codex 正在思考'));
      box.innerHTML = '<i></i><i></i><i></i>';
      thinking.appendChild(box);
      log.appendChild(thinking);
    }
    log.scrollTop = log.scrollHeight;
  }

  function updateChrome(data) {
    var login = data.login || {};
    var text, cls = '';
    if (login.state === 'starting' || login.state === 'pending') {
      text = t('等待授权'); cls = 'warn';
    } else if (data.logged_in) {
      text = t('已登录'); cls = 'ok';
    } else {
      text = t('未登录');
    }
    badge.textContent = text;
    badge.className = 'codex-badge ' + cls;
    stopBtn.hidden = !data.busy;
    send.disabled = sending || data.busy;
    var parts = [];
    if (data.settings && data.settings.sandbox) parts.push(t('沙箱') + ': ' + data.settings.sandbox);
    if (data.thread_id) parts.push(t('会话') + ': ' + data.thread_id.slice(0, 8));
    if (data.records && data.records.files) parts.push(t('记录') + ': ' + data.records.files + ' ' + t('个文件'));
    if (!data.logged_in) parts.push(t('请先在配置页登录 Codex'));
    meta.textContent = parts.join(' · ');
  }

  async function refresh(signal) {
    /* Status is read-only: the page's own Basic Auth credential suffices.
       authFetch() would pop the admin login dialog on every dashboard load. */
    var data = await App.json('/api/codex/status',
      {timeout: 15000, signal: signal});
    lastData = data;
    render(data);
    updateChrome(data);
  }

  App.poll(refresh, {interval: 1500});
  document.addEventListener('languagechange', function () { signature = ''; if (lastData) render(lastData); });

  form.addEventListener('submit', async function (event) {
    event.preventDefault();
    var text = input.value.trim();
    if (!text || sending) return;
    sending = true; send.disabled = true;
    try {
      var response = await authFetch('/api/codex/chat', {
        method: 'POST', headers: {'Content-Type': 'text/plain;charset=UTF-8'}, body: text
      });
      var result = await response.json();
      if (!response.ok || !result.ok) throw new Error(result.error || ('HTTP ' + response.status));
      input.value = '';
      signature = '';
      await refresh();
    } catch (error) {
      App.status('codex_chat', 'err', t('发送失败') + ': ' + error.message);
    } finally {
      sending = false;
      send.disabled = false;
    }
  });

  input.addEventListener('keydown', function (event) {
    if (event.key === 'Enter' && !event.shiftKey && !event.isComposing) {
      event.preventDefault();
      form.requestSubmit ? form.requestSubmit() : form.dispatchEvent(new Event('submit'));
    }
  });

  stopBtn.addEventListener('click', async function () {
    stopBtn.disabled = true;
    try {
      await authFetch('/api/codex/stop', {method: 'POST'});
      App.status('codex_chat', 'ok', t('已停止生成'));
      signature = '';
      await refresh();
    } catch (error) {
      App.status('codex_chat', 'err', t('停止失败'));
    } finally { stopBtn.disabled = false; }
  });
})();
