// SPDX-License-Identifier: GPL-3.0-or-later
(function () {
  var tbody = document.getElementById('rows_detections');
  var button = document.getElementById('refresh_detections');
  var panel = document.getElementById('detections_panel');
  var busy = false;
  function message(key) {
    tbody.replaceChildren();
    var cell = tbody.insertRow().insertCell(); cell.colSpan = 6;
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
      var response = await authFetch('/api/detections');
      if (!response.ok) throw new Error('HTTP ' + response.status);
      var data = await response.json();
      if (!data.files || !data.files.length) { message('暂无识别结果'); return; }
      tbody.replaceChildren();
      data.files.forEach(function (file) {
        var row = tbody.insertRow();
        row.insertCell().textContent = file.date;
        row.insertCell().textContent = file.name.split('/').pop();
        row.insertCell().textContent = String(file.objects);
        var focus = row.insertCell();
        if (file.focus_position_raw == null) setI18nText(focus, '未知');
        else focus.textContent = String(file.focus_position_raw);
        row.insertCell().textContent = (file.size / 1024).toFixed(1) + ' KB';
        var links = row.insertCell();
        downloadLink(links, file.name, '.jpg', '下载照片');
        links.appendChild(document.createTextNode(' '));
        downloadLink(links, file.name, '.json', '下载检测数据');
      });
    } catch (error) { message('加载失败（需管理员权限）'); }
    finally { busy = false; button.disabled = false; }
  }
  button.addEventListener('click', load);
  document.addEventListener('filestabchange', function (event) { if (event.detail.tab === 'detections') load(); });
  var timer = setInterval(load, 5000);
  window.addEventListener('pagehide', function () { clearInterval(timer); });
})();
