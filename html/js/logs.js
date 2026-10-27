// SPDX-License-Identifier: GPL-3.0-or-later
/* logs.js — 日志/录像列表、下载、删除页面逻辑。
 * 日志与录像共用 loadFileList/delFile（复用同一套渲染与删除流程）；
 * 打包下载统一走 authFetch（避免 <a> 直连未登录时弹浏览器原生认证框）。 */
(function() {
  var splitBusy = false;
  var splitButton = document.getElementById('split_rec_btn');

  function updateSplitButton(status) {
    if (!splitButton) return;
    var pending = splitBusy || !!(status && status.split_pending);
    splitButton.disabled = pending || !status || !status.recording || !status.frames;
    setI18nText(splitButton, pending ? '正在截断...' : '截断并续录');
  }

  function fmtSize(bytes) {
    var k = bytes / 1024;
    if (k >= 1024) return (k / 1024).toFixed(1) + ' MB';
    return k.toFixed(1) + ' KB';
  }

  /* 轻量提示（替代 alert） */
  var toastTimer = null;
  function toast(msg, ok, values) {
    var t = document.getElementById('toast');
    if (!t) { alert(window.t(msg, values)); return; }
    setI18nText(t, msg, values);
    t.className = 'toast show ' + (ok ? 'toast-ok' : 'toast-err');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function() { t.className = 'toast'; }, 2500);
  }
  window.fileToast = toast;

  /* 拆分 "日期/文件名" 两段式 name */
  function splitName(name) {
    var slash = name.indexOf('/');
    return slash >= 0 ? [name.slice(0, slash), name.slice(slash + 1)] : ['', name];
  }

  /* 通用表格行渲染：下载用 href（GET），删除用 delFn 回调；active 行标记"录制中" */
  function addRow(tbody, f, dlUrl, delFn, active) {
    var parts = splitName(f.name);
    var date = parts[0], file = parts[1];

    var tr = document.createElement('tr');
    if (active) tr.className = 'row-active';

    var tdDate = document.createElement('td');
    tdDate.textContent = date;

    var tdName = document.createElement('td');
    tdName.textContent = file;

    var tdSize = document.createElement('td');
    tdSize.textContent = fmtSize(f.size);

    var tdTime = document.createElement('td');
    tdTime.textContent = f.mtime;

    var tdOp = document.createElement('td');
    var dl = document.createElement('a');
    dl.className = 'dl';
    dl.href = dlUrl + encodeURIComponent(f.name);
    setI18nText(dl, '下载');
    if (active) {
      dl.removeAttribute('href');
      dl.setAttribute('aria-disabled', 'true');
      dl.setAttribute('data-i18n-title', '当前录像需先截断保存后下载');
      dl.title = t('当前录像需先截断保存后下载');
    }
    var del = document.createElement('button');
    del.className = 'del';
    setI18nText(del, active ? '录制中' : '删除');
    if (active) {
      del.disabled = true;   /* 后端对正在录制的文件返回 409 */
      del.title = t('正在录制的文件无法删除');
      del.setAttribute('data-i18n-title', '正在录制的文件无法删除');
    }
    del.addEventListener('click', delFn);
    tdOp.appendChild(dl);
    tdOp.appendChild(del);

    tr.appendChild(tdDate);
    tr.appendChild(tdName);
    tr.appendChild(tdSize);
    tr.appendChild(tdTime);
    tr.appendChild(tdOp);
    tbody.appendChild(tr);
  }

  /* 通用列表加载（日志/录像共用）：
     apiUrl 返回 {files:[{name,size,mtime}]}；
     del = {url, method, reload}，activeFile 为"录制中"文件名（可空）。 */
  async function loadFileList(apiUrl, tbodyId, emptyMsg, dlBase, del, activeFile) {
    var tbody = document.getElementById(tbodyId);
    function message(key, cls) {
      tbody.innerHTML = '<tr><td colspan="5"></td></tr>';
      tbody.firstChild.firstChild.className = cls;
      setI18nText(tbody.firstChild.firstChild, key);
    }
    message('加载中…', 'loading');
    var files;
    try {
      var r = await fetch(apiUrl);
      if (r.status === 401) r = await authFetch(apiUrl);  /* 列表需要管理员认证，401 时统一登录重试 */
      if (!r.ok) throw new Error('load ' + r.status);
      var data = await r.json();
      files = (data.files || data.logs) || [];   /* 录像接口返回 files，日志接口返回 logs */
    } catch (e) {
      message('加载失败（需管理员权限）', 'empty');
      return;
    }
    if (files.length === 0) {
      message(emptyMsg, 'empty');
      return;
    }
    tbody.innerHTML = '';
    files.forEach(function(f) {
      addRow(tbody, f, dlBase, del ? function() {
        if (!confirm(t('删除文件 ') + f.name + t('？'))) return;
        var opt = { method: del.method };
        if (del.bodyName) opt.body = f.name;   /* 录像删除：文件名放请求体 */
        authFetch(del.makeUrl(f.name), opt)    /* 日志删除：文件名放 URL 路径 */
          .then(function(r) {
            if (r.ok) { toast('已删除 {file}', true, {file: f.name}); del.reload(); }
            else if (r.status === 401) toast('需要管理员权限（root 或 sudo 用户）', false);
            else if (r.status === 409) toast('该文件正在录制中，无法删除', false);
            else toast('删除失败 ({status})', false, {status: r.status});
          })
          .catch(function() {});
      } : null, activeFile === f.name);
    });
  }

  /* 打包下载：统一走 authFetch（带登录重试），成功转 blob 触发浏览器下载 */
  function bindPack(btn, url, filename) {
    btn.addEventListener('click', function(ev) {
      ev.preventDefault();
      setI18nText(btn, '打包中…');
      authFetch(url)
        .then(function(r) {
          if (!r.ok) throw new Error('pack ' + r.status);
          return r.blob();
        })
        .then(function(b) {
          var a = document.createElement('a');
          a.href = URL.createObjectURL(b);
          a.download = filename;
          a.click();
          URL.revokeObjectURL(a.href);
          setI18nText(btn, '📦 打包下载');
          toast('打包完成，开始下载', true);
        })
        .catch(function() {
          setI18nText(btn, '📦 打包下载');
          toast('打包失败或已取消', false);
        });
    });
  }

  /* ---- 日志列表 ---- */
  function loadLogs() {
    return loadFileList('/api/logs', 'rows_logs', '暂无日志文件', '/logfile/',
      { makeUrl: function(n) { return '/logfile/' + encodeURIComponent(n); },
        method: 'DELETE', reload: loadLogs });
  }

  /* ---- 录像列表（按天分目录，自动录制持续生成，周期性刷新） ---- */
  async function loadRec() {
    var activeFile = '';
    try {
      var st = await (await fetch('/api/rec/status')).json();
      updateSplitButton(st);
      if (st.recording) activeFile = st.file;   /* 格式与列表项 name 一致 */
    } catch (e) { updateSplitButton(null); }
    return loadFileList('/api/rec/list', 'rows_rec', '暂无录像文件', '/recfile/',
      { makeUrl: function() { return '/api/rec/delete'; },
        method: 'POST', bodyName: true, reload: loadRec }, activeFile);
  }

  if (splitButton) splitButton.addEventListener('click', async function() {
    if (splitBusy) return;
    splitBusy = true;
    updateSplitButton(null);
    try {
      var status = await (await fetch('/api/rec/status')).json();
      if (!status.recording || !status.file) {
        toast('当前没有正在录制的视频', false); return;
      }
      var previousFile = status.file;
      var response = await authFetch('/api/rec/split', {
        method: 'POST', headers: {'Content-Type': 'text/plain;charset=UTF-8'}, body: previousFile
      });
      if (!response.ok) {
        toast(response.status === 409 ? '录像状态已变化，请刷新后重试' : '截断失败 ({status})', false, {status: response.status});
        return;
      }
      for (var attempt = 0; attempt < 50; attempt++) {
        await new Promise(function(resolve) { setTimeout(resolve, 200); });
        status = await (await fetch('/api/rec/status')).json();
        if (status.split_error) { toast('录像分段保存失败，请检查磁盘与日志', false); return; }
        if (status.recording && status.file && status.file !== previousFile && !status.split_pending && status.frames > 0) {
          toast('已保存 {file}，正在继续录制', true, {file: previousFile});
          return;
        }
        if (!status.recording && !status.split_pending) {
          toast('录像已停止，请检查设备状态', false); return;
        }
      }
      toast('分段请求已提交，正在等待关键帧', true);
    } catch(e) {
      if (e.message !== 'auth cancelled') toast('截断请求失败', false);
    } finally {
      splitBusy = false;
      await loadRec();
    }
  });

  /* ---- 一键清空（清空录像时保留正在录制的文件） ---- */
  function bindClear(btn, url, confirmMsg, resultKey, reload) {
    btn.addEventListener('click', function() {
      if (!confirm(t(confirmMsg))) return;
      authFetch(url, { method: 'POST' })
        .then(function(r) {
          if (r.ok) {
            r.json().then(function(j) { toast(resultKey, true, {count: j.deleted || 0}); });
            reload();
          } else if (r.status === 401) toast('需要管理员权限（root 或 sudo 用户）', false);
          else toast('清空失败 ({status})', false, {status: r.status});
        })
        .catch(function() {});
    });
  }
  bindClear(document.getElementById('clear_btn'), '/api/logs/clear',
    '确定清空全部日志？此操作不可恢复', '已清空 {count} 个日志文件', loadLogs);
  bindClear(document.getElementById('clear_rec_btn'), '/api/rec/clear',
    '确定清空全部录像？此操作不可恢复', '已清空 {count} 个录像文件', loadRec);

  /* ---- Tab 切换 ---- */
  function switchTab(tab) {
    var isRec = tab === 'rec';
    var isLogs = tab === 'logs';
    document.getElementById('tbl_logs').style.display = isLogs ? '' : 'none';
    document.getElementById('tbl_rec').style.display = isRec ? '' : 'none';
    document.getElementById('detections_panel').style.display = tab === 'detections' ? '' : 'none';
    document.getElementById('pack_btn').style.display = isLogs ? '' : 'none';
    document.getElementById('pack_rec_btn').style.display = isRec ? '' : 'none';
    document.getElementById('clear_btn').style.display = isLogs ? '' : 'none';
    document.getElementById('clear_rec_btn').style.display = isRec ? '' : 'none';
    if (splitButton) splitButton.style.display = isRec ? '' : 'none';
    var btns = document.querySelectorAll('.tab-btn');
    for (var i = 0; i < btns.length; i++)
      btns[i].classList.toggle('tab-on', btns[i].getAttribute('data-tab') === tab);
    document.dispatchEvent(new CustomEvent('filestabchange', {detail:{tab:tab}}));
  }
  window.switchTab = switchTab;

  bindPack(document.getElementById('pack_btn'), '/logs/pack', 'logs_pack.tar.gz');
  bindPack(document.getElementById('pack_rec_btn'), '/api/rec/pack', 'recordings_pack.tar.gz');

  loadLogs();
  loadRec();
  setInterval(loadRec, 5000);   /* 录像持续生成，低频刷新 */
})();
