// SPDX-License-Identifier: GPL-3.0-or-later
(function () {
  var reports = document.getElementById('rows_detection_reports');
  var packButton = document.getElementById('pack_detections_btn');
  var clearButton = document.getElementById('clear_detections_btn');
  var busy = false;
  var refreshPending = false;
  var bulkBusy = false;
  function pageBusy() { return window.carFiles.isBulkBusy(); }
  function setBulkBusy(value) {
    bulkBusy = value;
    clearButton.disabled = value;
    packButton.setAttribute('aria-disabled', String(value));
  }
  function saveDownload(blob, filename) {
    var anchor = document.createElement('a');
    var url = URL.createObjectURL(blob);
    anchor.href = url; anchor.download = filename;
    document.body.appendChild(anchor); anchor.click(); anchor.remove();
    setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
  }
  async function reportDownload(url) {
    for (var attempt=0; ; attempt++) {
      var response=await authFetch(url);
      if ((response.status!==409 && response.status!==503) || attempt>=4) return response;
      await new Promise(function (resolve) { setTimeout(resolve,500*(attempt+1)); });
    }
  }
  function reportBusy(response) {
    if (response.status!==409 && response.status!==503) return false;
    window.fileToast('结果正在更新，请稍后重试',false);
    return true;
  }
  function message(key) {
    reports.replaceChildren();
    var cell = reports.insertRow().insertCell(); cell.colSpan = 5;
    setI18nText(cell, key);
  }
  function downloadLink(cell, name, suffix, key) {
    var link = document.createElement('a'); link.className = 'dl';
    link.href = '/detectionfile/' + name.split('/').map(encodeURIComponent).join('/') + suffix;
    setI18nText(link, key);
    link.addEventListener('click', async function (event) {
      event.preventDefault();
      if (bulkBusy || pageBusy() || link.getAttribute('aria-disabled') === 'true') return;
      setBulkBusy(true);
      link.setAttribute('aria-disabled','true');
      try {
        var response = await reportDownload(link.href);
        if (reportBusy(response)) return;
        if (!response.ok) throw new Error('HTTP ' + response.status);
        saveDownload(await response.blob(), name.split('/').pop() + suffix);
      } catch (error) {
        if (error.message !== 'auth cancelled') alert(t('结果下载失败，请重试'));
      } finally { link.removeAttribute('aria-disabled'); setBulkBusy(false); }
    });
    cell.appendChild(link);
  }
  async function load() {
    if (document.hidden) return;
    if (busy) { refreshPending = true; return; }
    busy = true;
    try {
      var response = await authFetch('/api/detections/reports');
      if (!response.ok) throw new Error('Unable to load saved detections');
      var data = await response.json();
      reports.replaceChildren();
      if (!data.files || !data.files.length) {
        var empty = reports.insertRow().insertCell(); empty.colSpan = 5; setI18nText(empty, '暂无结果文件');
      } else data.files.forEach(function (report) {
        var row = reports.insertRow();
        row.insertCell().textContent = window.carFiles.formatDate(report.date, report.mtime);
        var filename = row.insertCell();
        filename.textContent = report.name.split('/').pop(); filename.title = filename.textContent;
        row.insertCell().textContent = window.carFiles.formatSize(report.size);
        row.insertCell().textContent = report.mtime || '—';
        var actions = row.insertCell();
        window.carFiles.addPreviewButton(actions, 'result', report.name, false);
        downloadLink(actions, report.name, '', '下载');
        var remove = document.createElement('button');
        remove.className = 'del'; setI18nText(remove, '删除');
        remove.addEventListener('click', async function () {
          if (bulkBusy || pageBusy()) return;
          if (!confirm(t('删除日报 {file}？后续检测将继续创建当天文件。', {file: report.name}))) return;
          remove.disabled = true;
          try {
            var response = await authFetch('/api/detections/reports/delete', {
              method:'POST', headers:{'Content-Type':'text/plain;charset=UTF-8'}, body:report.name
            });
            if (!response.ok) throw new Error('HTTP ' + response.status);
            window.fileToast('已删除 {file}', true, {file:report.name});
            await load();
          } catch (error) {
            if (error.message !== 'auth cancelled') window.fileToast('删除失败', false);
          } finally { remove.disabled = false; }
        });
        actions.appendChild(remove);
      });
    } catch (error) {
      message('加载失败（需管理员权限）');
    }
    finally {
      busy = false;
      if (refreshPending) { refreshPending = false; load(); }
    }
  }
  packButton.addEventListener('click', async function (event) {
    event.preventDefault();
    if (bulkBusy || pageBusy()) return;
    setBulkBusy(true); setI18nText(packButton, '打包中…');
    try {
      var response = await reportDownload('/api/detections/reports/pack');
      if (response.status === 404) { window.fileToast('暂无可打包的结果', false); return; }
      if (reportBusy(response)) return;
      if (!response.ok) throw new Error('HTTP ' + response.status);
      saveDownload(await response.blob(), 'detections_pack.tar.gz');
      window.fileToast('打包完成，开始下载', true);
    } catch (error) {
      if (error.message !== 'auth cancelled') window.fileToast('打包失败或已取消', false);
    } finally {
      setI18nText(packButton, '📦 打包下载'); setBulkBusy(false);
    }
  });
  clearButton.addEventListener('click', async function () {
    if (bulkBusy || pageBusy() || !confirm(t('确定清空全部结果？此操作不可恢复，后续检测将继续创建当天文件。'))) return;
    setBulkBusy(true);
    try {
      var response = await authFetch('/api/detections/reports/clear', {method:'POST'});
      if (!response.ok) {
        window.fileToast(response.status === 401 ? '需要管理员权限（root 或 sudo 用户）' : '清空失败 ({status})',
          false, {status:response.status});
        return;
      }
      var result = await response.json();
      window.fileToast('已清空 {count} 个结果文件', true, {count:result.deleted || 0});
      await load();
    } catch (error) {
      if (error.message !== 'auth cancelled') window.fileToast('清空结果失败，请重试', false);
    } finally { setBulkBusy(false); }
  });
  load();
  document.addEventListener('fileschanged', load);
  var timer = setInterval(load, 5000);
  window.addEventListener('pagehide', function () { clearInterval(timer); });
})();
