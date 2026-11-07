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
    cell.colSpan = 6;
    setI18nText(cell, key);
  }


  function previewBox() {
    var box = document.getElementById('codex_preview');
    if (box) return box;
    box = document.createElement('div');
    box.id = 'codex_preview';
    box.style.cssText = 'position:fixed;inset:0;z-index:1000;display:none;' +
      'align-items:center;justify-content:center;background:rgba(4,8,20,.72);padding:24px';
    box.innerHTML = '<div style="background:var(--card-bg);border:1px solid var(--card-border2);' +
      'border-radius:16px;padding:16px;max-width:min(900px,92vw);max-height:84vh;display:flex;' +
      'flex-direction:column;gap:10px"><div style="display:flex;justify-content:space-between;' +
      'align-items:center;gap:12px"><b id="codex_preview_name" style="font-size:13px"></b>' +
      '<button type="button" id="codex_preview_close" class="btn btn-back">关闭</button></div>' +
      '<pre id="codex_preview_body" style="margin:0;overflow:auto;font-size:12px;line-height:1.6;' +
      'white-space:pre-wrap;color:var(--text)"></pre></div>';
    document.body.appendChild(box);
    box.addEventListener('click', function (e) { if (e.target === box) box.style.display = 'none'; });
    box.querySelector('#codex_preview_close').addEventListener('click', function () {
      box.style.display = 'none';
    });
    return box;
  }

  async function preview(file) {
    try {
      var r = await authFetch('/api/codex/record?name=' + encodeURIComponent(file.name));
      if (!r.ok) throw new Error('HTTP ' + r.status);
      var text = await r.text();
      var box = previewBox();
      box.querySelector('#codex_preview_name').textContent = file.name;
      box.querySelector('#codex_preview_body').textContent = text.slice(0, 200000);
      box.style.display = 'flex';
    } catch (error) {
      if (error.message !== 'auth cancelled') window.fileToast(t('预览失败'), false);
    }
  }

  async function remove(file) {
    if (!confirm(t('删除 {name}？此操作不可恢复。', {name: file.name}))) return;
    try {
      var r = await authFetch('/api/codex/delrecord?name=' + encodeURIComponent(file.name),
                              {method: 'POST'});
      if (!r.ok) throw new Error('HTTP ' + r.status);
      var j = await r.json();
      if (!j.ok) throw new Error(j.error || 'failed');
      window.fileToast(t('已删除 {name}', {name: file.name}), true);
      await load();
    } catch (error) {
      if (error.message !== 'auth cancelled') window.fileToast(t('删除失败'), false);
    }
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
        row.insertCell().textContent = file.ctime || '—';
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
        var pv = document.createElement('button');
        pv.type = 'button';
        pv.className = 'file-preview-button';
        setI18nText(pv, '预览');
        pv.addEventListener('click', function () { preview(file); });
        actions.appendChild(pv);
        actions.appendChild(link);
        var del = document.createElement('button');
        del.type = 'button';
        del.className = 'del';
        setI18nText(del, '删除');
        del.addEventListener('click', function () { remove(file); });
        actions.appendChild(del);
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
