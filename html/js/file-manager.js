// SPDX-License-Identifier: GPL-3.0-or-later
(function() {
  var files = window.gatewayFiles = window.gatewayFiles || {};
  var pack = document.getElementById('pack_all_files_btn');
  var clear = document.getElementById('clear_all_files_btn');
  var overlay = document.getElementById('file_preview_overlay');
  var close = document.getElementById('file_preview_close');
  var filename = document.getElementById('file_preview_name');
  var status = document.getElementById('file_preview_status');
  var text = document.getElementById('file_preview_text');
  var video = document.getElementById('file_preview_video');
  var bulkBusy = false;
  var previewGeneration = 0;
  var controller = null;
  var videoSession = false;
  var sessionCleanup = Promise.resolve();
  var previousFocus = null;

  function busy(value) {
    bulkBusy = value;
    clear.disabled = value;
    pack.setAttribute('aria-disabled', String(value));
  }
  files.isBulkBusy = function() { return bulkBusy; };

  function save(blob, name) {
    var anchor = document.createElement('a');
    var url = URL.createObjectURL(blob);
    anchor.href = url; anchor.download = name;
    document.body.appendChild(anchor); anchor.click(); anchor.remove();
    setTimeout(function() { URL.revokeObjectURL(url); }, 1000);
  }

  pack.addEventListener('click', async function(event) {
    event.preventDefault();
    if (bulkBusy) return;
    busy(true); setI18nText(pack, '正在打包全部文件…');
    try {
      var response = await authFetch('/api/files/download-session', {method:'POST'});
      if (!response.ok) throw new Error('HTTP ' + response.status);
      // Native download streams large archives to disk instead of retaining
      // all recordings in a JavaScript Blob before the save dialog appears.
      var anchor = document.createElement('a');
      anchor.href = '/api/files/pack'; anchor.download = 'gateway_files.tar.gz';
      document.body.appendChild(anchor); anchor.click(); anchor.remove();
      window.fileToast('已开始打包下载，请在浏览器下载列表查看进度', true);
    } catch(error) {
      if (error.message !== 'auth cancelled') window.fileToast('打包失败或已取消', false);
    } finally { setI18nText(pack, '打包全部文件'); setTimeout(function() { busy(false); }, 1500); }
  });

  clear.addEventListener('click', async function() {
    if (bulkBusy || (files.isRecordingBusy && files.isRecordingBusy())) return;
    if (!confirm(t('确定删除全部日志、录像和结果？此操作不可恢复，正在录制的文件会保留。'))) return;
    busy(true);
    try {
      var response = await authFetch('/api/files/clear', {method:'POST'});
      if (!response.ok) throw new Error('HTTP ' + response.status);
      var result = await response.json();
      window.fileToast('已删除 {count} 个文件，正在录制的文件已保留', true, {count:result.deleted || 0});
      document.dispatchEvent(new CustomEvent('fileschanged'));
    } catch(error) {
      if (error.message !== 'auth cancelled') window.fileToast('删除全部文件失败，请重试', false);
    } finally { busy(false); }
  });

  function closePreview() {
    previewGeneration++;
    if (controller) { controller.abort(); controller = null; }
    video.pause(); video.removeAttribute('src'); video.load(); video.hidden = true;
    text.textContent = ''; text.hidden = true;
    overlay.hidden = true;
    if (videoSession) {
      videoSession = false;
      sessionCleanup = authFetch('/api/files/video-session', {method:'DELETE',keepalive:true}).catch(function() {});
    }
    if (previousFocus && previousFocus.isConnected) previousFocus.focus();
  }

  async function preview(type, name, source) {
    closePreview();
    previousFocus = source;
    var generation = previewGeneration;
    controller = new AbortController();
    var signal = controller.signal;
    filename.textContent = name;
    overlay.hidden = false;
    setI18nText(status, '正在加载预览…');
    close.focus();
    try {
      if (type === 'recording') {
        await sessionCleanup;
        if (generation !== previewGeneration) return;
        var session = await authFetch('/api/files/video-session', {
          method:'POST',headers:{'Content-Type':'text/plain;charset=UTF-8'},body:name,signal:signal
        });
        if (!session.ok) throw new Error('HTTP ' + session.status);
        if (generation !== previewGeneration) return;
        videoSession = true;
        video.hidden = false;
        video.src = '/api/files/video?name=' + encodeURIComponent(name);
        video.load();
        setI18nText(status, '可播放已保存的录像');
      } else {
        var response = await authFetch('/api/files/preview?type=' + encodeURIComponent(type) + '&name=' + encodeURIComponent(name), {signal:signal});
        if (!response.ok) throw new Error('HTTP ' + response.status);
        var result = await response.json();
        if (generation !== previewGeneration) return;
        text.textContent = String(result.text || '');
        text.hidden = false;
        var size = files.formatSize ? files.formatSize(result.total_bytes) : String(result.total_bytes || 0) + ' B';
        setI18nText(status, result.truncated ? '预览已截断，文件总大小 {size}' : '文件大小 {size}', {size:size});
      }
    } catch(error) {
      if (generation !== previewGeneration || error.name === 'AbortError') return;
      setI18nText(status, error.message === 'auth cancelled' ? '预览已取消' : '无法预览此文件，请重试');
    }
  }

  files.addPreviewButton = function(cell, type, name, active) {
    var button = document.createElement('button');
    button.type = 'button'; button.className = 'file-preview-button';
    setI18nText(button, '预览');
    if (active) {
      button.disabled = true;
      button.setAttribute('data-i18n-title', '请先下载以保存当前录像片段');
      button.title = t('请先下载以保存当前录像片段');
    }
    button.addEventListener('click', function() { preview(type, name, button); });
    cell.appendChild(button);
  };
  close.addEventListener('click', closePreview);
  overlay.addEventListener('click', function(event) { if (event.target === overlay) closePreview(); });
  document.addEventListener('keydown', function(event) {
    if (overlay.hidden) return;
    if (event.key === 'Escape') { event.preventDefault(); closePreview(); }
    else if (event.key === 'Tab') {
      var controls = overlay.querySelectorAll('button:not(:disabled),video:not([hidden])');
      var first = controls[0], last = controls[controls.length-1];
      if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last.focus(); }
      else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus(); }
    }
  });
  video.addEventListener('error', function() {
    if (!overlay.hidden && video.getAttribute('src')) setI18nText(status, '无法播放此录像，请检查文件');
  });
  window.addEventListener('pagehide', closePreview);
})();
