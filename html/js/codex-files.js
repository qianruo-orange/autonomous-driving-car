// SPDX-License-Identifier: GPL-3.0-or-later
/* codex-files.js — Codex interaction records on the file download page. */
(function () {
  'use strict';
  var rows = document.getElementById('rows_codex');
  var packButton = document.getElementById('pack_codex_btn');
  var clearButton = document.getElementById('clear_codex_btn');
  if (!rows || !packButton || !clearButton) return;
  var busy = false;

  function message(key) {
    rows.replaceChildren();
    var cell = rows.insertRow().insertCell();
    cell.colSpan = 5;
    setI18nText(cell, key);
  }

  async function load() {
    if (document.hidden || busy) return;
    busy = true;
    try {
      /* Listing is read-only: use the page's Basic Auth credential so the
         admin login dialog only appears for real write/download actions. */
      var data = await App.json('/api/codex/records');
      rows.replaceChildren();
      if (!data.files || !data.files.length) {
        message('暂无交互记录');
      } else data.files.forEach(function (file) {
        var row = rows.insertRow();
        row.insertCell().textContent = file.date || '—';
        var nameCell = row.insertCell();
        nameCell.textContent = file.name;
        nameCell.title = file.name;
        row.insertCell().textContent = window.carFiles.formatSize(file.size);
        row.insertCell().textContent = file.mtime || '—';
        var actions = row.insertCell();
        var link = document.createElement('a');
        link.className = 'dl';
        link.href = '/api/codex/record?name=' + encodeURIComponent(file.name);
        setI18nText(link, '下载');
        link.addEventListener('click', async function (event) {
          event.preventDefault();
          if (busy) return;
          try {
            var response = await authFetch(link.href);
            if (!response.ok) throw new Error('HTTP ' + response.status);
            App.downloadBlob(await response.blob(), file.name);
            window.fileToast(t('记录已开始下载'), true);
          } catch (error) {
            if (error.message !== 'auth cancelled') window.fileToast(t('记录下载失败'), false);
          }
        });
        actions.appendChild(link);
      });
    } catch (error) {
      message('加载失败（需管理员权限）');
    } finally { busy = false; }
  }

  packButton.addEventListener('click', async function (event) {
    event.preventDefault();
    if (busy) return;
    busy = true;
    setI18nText(packButton, '打包中…');
    try {
      var response = await authFetch('/api/codex/export');
      if (!response.ok) throw new Error('HTTP ' + response.status);
      var disposition = response.headers.get('Content-Disposition') || '';
      var match = /filename="?([^";]+)"?/.exec(disposition);
      App.downloadBlob(await response.blob(), match ? match[1] : 'codex_chat.jsonl');
      window.fileToast(t('完整交互记录已开始下载'), true);
    } catch (error) {
      if (error.message !== 'auth cancelled') window.fileToast(t('打包失败'), false);
    } finally {
      setI18nText(packButton, '📦 打包下载');
      busy = false;
    }
  });

  clearButton.addEventListener('click', async function () {
    if (busy || !confirm(t('确定清空全部 Codex 交互记录？此操作不可恢复。'))) return;
    busy = true;
    try {
      var response = await authFetch('/api/codex/records/clear', {method: 'POST'});
      if (!response.ok) throw new Error('HTTP ' + response.status);
      var result = await response.json();
      window.fileToast(t('已清空 {count} 个记录文件'), true, {count: result.deleted || 0});
      await load();
    } catch (error) {
      if (error.message !== 'auth cancelled') window.fileToast(t('清空记录失败'), false);
    } finally { busy = false; }
  });

  load();
  document.addEventListener('fileschanged', load);
  var timer = setInterval(load, 5000);
  window.addEventListener('pagehide', function () { clearInterval(timer); });
})();
