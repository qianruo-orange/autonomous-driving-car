// SPDX-License-Identifier: GPL-3.0-or-later
(function () {
  var reports = document.getElementById('rows_detection_reports');
  var button = document.getElementById('refresh_detections');
  var panel = document.getElementById('detections_panel');
  var busy = false;
  function message(key) {
    reports.replaceChildren();
    var cell = reports.insertRow().insertCell(); cell.colSpan = 4;
    setI18nText(cell, key);
  }
  function downloadLink(cell, name, suffix, key) {
    var link = document.createElement('a'); link.className = 'dl';
    link.href = '/detectionfile/' + name.split('/').map(encodeURIComponent).join('/') + suffix;
    setI18nText(link, key);
    link.addEventListener('click', async function (event) {
      event.preventDefault();
      if (link.getAttribute('aria-disabled') === 'true') return;
      link.setAttribute('aria-disabled','true');
      try {
        var response = await authFetch(link.href);
        if (!response.ok) throw new Error('HTTP ' + response.status);
        var blob = await response.blob();
        var anchor = document.createElement('a');
        var url = URL.createObjectURL(blob);
        anchor.href = url; anchor.download = name.split('/').pop() + suffix;
        document.body.appendChild(anchor); anchor.click(); anchor.remove();
        setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
      } catch (error) {
        if (error.message !== 'auth cancelled') alert(t('识别结果下载失败，请重试'));
      } finally { link.removeAttribute('aria-disabled'); }
    });
    cell.appendChild(link);
  }
  async function load() {
    if (busy || document.hidden || panel.style.display === 'none') return;
    busy = true; button.disabled = true;
    try {
      var response = await authFetch('/api/detections/reports');
      if (!response.ok) throw new Error('Unable to load saved detections');
      var data = await response.json();
      reports.replaceChildren();
      if (!data.files || !data.files.length) {
        var empty = reports.insertRow().insertCell(); empty.colSpan = 4; setI18nText(empty, '暂无检测汇总');
      } else data.files.forEach(function (report) {
        var row = reports.insertRow();
        row.insertCell().textContent = report.date;
        row.insertCell().textContent = report.name.split('/').pop();
        row.insertCell().textContent = (report.size / 1024).toFixed(1) + ' KB';
        var actions = row.insertCell();
        downloadLink(actions, report.name, '', '下载');
        var remove = document.createElement('button');
        remove.className = 'del'; setI18nText(remove, '删除');
        remove.addEventListener('click', async function () {
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
    finally { busy = false; button.disabled = false; }
  }
  button.addEventListener('click', load);
  document.addEventListener('filestabchange', function (event) { if (event.detail.tab === 'detections') load(); });
  var timer = setInterval(load, 5000);
  window.addEventListener('pagehide', function () { clearInterval(timer); });
})();
