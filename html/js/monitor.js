// SPDX-License-Identifier: GPL-3.0-or-later
/* monitor.js — 仪表盘数据刷新 */
(function() {
  /* Same module serves the dashboard (/) and the status page (/status).
     The status page has no <video>/<img>: skip the player + FPS loops there. */
  var HAS_VIDEO = !!document.getElementById('stream_video');
  var lastCpu = {user:0,sys:0,idle:0,total:0};
  var lastCores = [];
  var npuCount = -1;
  var cpuCount = -1;   /* 已渲染的 CPU 核数，变化时重建核条 */

  /* 网络速度跟踪 */
  var lastNet = {eth0:{rx:0,tx:0,ts:0},wlan0:{rx:0,tx:0,ts:0}};
  var netInited = false;

  /* 视频流：单条 fetch 连接同时喂 <img>（blob URL 逐帧）并扫描 SOI(FF D8) 计数。
     旧方案是 <img src> 显示 + 独立 fetch 计数双路下载同一流（流量×2），
     浏览器/无线吃紧时会间歇掉速；单路方案显示与计数天然一致 */
  var fpsCnt = 0, fpsWin = Date.now(), fpsVal = 0, shownCnt = 0;
  var receivedFps = 0;
  var cameraFps = 0, processingAgeMs = 0, sourceDrops = 0, videoStatsBusy = false;
  var streamUrl = '', streamAbort = null, streamRunning = false;
  var imgEl = null, lastBlobUrl = null;
  var imgBusy = false;      /* 上一帧还在解码：不覆盖 src（覆盖会取消其加载） */
  var pendingBlob = null;   /* 解码期间收到的最新帧，仅保留最后一张，解码完立即补显 */
  var pendingBlobSeq = 0;   /* 与该帧对应的采集序号（X-Seq） */
  var pendingBlobTs = 0;    /* 与该帧对应的采集时刻（X-Ts，板端单调毫秒） */

  /* ---- MJPEG 透传链路的延迟/丢帧统计 ----
     透传链路没有 MSE buffered/playbackQuality，改用每帧自带的 X-Seq / X-Ts：
       · 丢帧 = 相邻两帧序号跳号（seq 由采集环单调递增，跳号只可能是没发出来/没收到）
       · 延迟 = 屏上这帧的采集时刻到它真正上屏的间隔。板端时间戳是单调时钟毫秒，
         与浏览器 performance.now() 不同源，故用 /api/video/stats 的 now_ms 折算钟差
         （一次往返估一次，取窗口内最小值抑制回程抖动）。 */
  var mjLastSeq = 0;         /* 上一帧收到的序号 */
  var mjDropTotal = 0;       /* 收到的帧里累计跳过的帧数 */
  var mjDropPerSec = 0;      /* 每秒丢帧（1s 窗口差值） */
  var mjDropMark = null;     /* 上一窗口的累计值 */
  var mjLatencyMs = null;    /* 采集→上屏端到端延迟（指数平滑） */
  var srvOffsets = [];       /* 最近的钟差样本（板端时钟 - 本机 perf.now） */

  /* ---- 检测框叠加：MJPEG 透传链路下画面是相机原始帧，服务端不再烧框，
     前端用每帧的 X-Seq 与 /api/detections/live 返回的 seq 对齐后画在 canvas 上 ---- */
  var detMap = new Map(), detLastSeq = 0, detPollBusy = false;
  var overlayCanvas = null, overlayCtx = null;
  var detTimer = null;
  /* 与烧框一致的类别配色，按类别号取模 */
  var DET_COLORS = ['#ff3b30', '#34c759', '#0a84ff', '#ff9f0a', '#bf5af2',
                    '#ffd60a', '#64d2ff', '#ff453a', '#30d158', '#5e5ce6'];

  function resetFps() {
    fpsCnt = 0; shownCnt = 0; fpsWin = Date.now(); fpsVal = 0;
    receivedFps = 0;
    mjLatencyMs = null;
    mjLastSeq = 0; mjDropTotal = 0;
    mjDropPerSec = 0; mjDropMark = null;
  }

  /* 显示一帧：仅在上一次 onload/onerror 之后调用，上一帧 URL 此时已被完整消费，
     可安全回收；onload 前就 revoke 会中断在途解码，弱设备上永远看不到画面 */
  function showFrame(blob, seq, ts) {
    var target = imgEl, session = streamAbort;
    if (!target || !session) return;
    var u = URL.createObjectURL(blob);
    imgBusy = true;
    target.onload = target.onerror = function(event) {
      if (streamAbort !== session || imgEl !== target) return;
      if (event.type === 'load') {
        shownCnt++;
        if (ts) recordLatency(ts);   /* 这一帧真正上屏了，才结算端到端延迟 */
        overlayDraw(seq);            /* 该帧已经在屏幕上，按同一 seq 叠框 */
      }
      imgBusy = false;
      if (pendingBlob) {          /* 解码慢于码流时丢弃中间帧，只补最新一帧 */
        var pb = pendingBlob, ps = pendingBlobSeq, pt = pendingBlobTs;
        pendingBlob = null;
        showFrame(pb, ps, pt);
      }
    };
    target.src = u;
    if (lastBlobUrl) URL.revokeObjectURL(lastBlobUrl);
    lastBlobUrl = u;
  }

  /* 采集时刻 → 上屏时刻的真实间隔。板端时钟与本机 performance.now() 差一个常量，
     用 srvOffsets（stats 轮询估出的钟差，取最小值）扣掉后即为端到端延迟 */
  function recordLatency(captureTs) {
    if (srvOffsets.length < 3) return;         /* 钟差样本不够，先显示 -- */
    var off = Math.max.apply(null, srvOffsets);   /* 板端毫秒 = 本机时间 + off（略偏小的下界） */
    /* 先把采集时刻折算到本机时间线（板端时刻 - off），再和现在相减 */
    var ms = performance.now() - (captureTs - off);
    if (!(ms >= 0)) ms = 0;                    /* 钟差抖动导致的微小负值 */
    if (ms > 3000) return;                     /* 暂停/切后台后的异常值，丢弃不参与平均 */
    mjLatencyMs = mjLatencyMs === null ? ms : mjLatencyMs * 0.7 + ms * 0.3;
  }

  function streamStop() {
    if (streamAbort) { streamAbort.abort(); streamAbort = null; }
    streamRunning = false;
    imgBusy = false;
    pendingBlob = null;
    if (lastBlobUrl) { URL.revokeObjectURL(lastBlobUrl); lastBlobUrl = null; }
    if (detTimer) { clearInterval(detTimer); detTimer = null; }
    detReset();
  }

  /* ---- 检测框叠加（透传链路专用） ---------------------------------------- */

  function detReset() {
    detMap.clear();
    detLastSeq = 0;
    overlayClear();
  }

  function overlayClear() {
    if (overlayCtx && overlayCanvas)
      overlayCtx.clearRect(0, 0, overlayCanvas.width, overlayCanvas.height);
  }

  /* 增量拉取：只取 seq 大于已持有最大值的记录，避免重复传输 */
  function detPoll() {
    if (detPollBusy || document.hidden || !streamRunning) return;
    detPollBusy = true;
    fetch('/api/detections/live?after=' + detLastSeq, {signal: AbortSignal.timeout(2000)})
      .then(function(r) { return r.ok ? r.json() : null; })
      .then(function(j) {
        if (!j || !j.items) return;
        for (var i = 0; i < j.items.length; i++) {
          var it = j.items[i];
          detMap.set(it.seq, it);
          if (it.seq > detLastSeq) detLastSeq = it.seq;
        }
        while (detMap.size > 24) detMap.delete(detMap.keys().next().value);
      })
      .catch(function() {})
      .then(function() { detPollBusy = false; });
  }

  /* 按显示中的帧序号取检测结果并绘制。
     坐标换算不再靠「假设 object-fit 是 contain 且内容居中」：直接读 <img> 的实际布局
     矩形和计算后的 object-fit，按内容真实位置换算。改样式、窗口拉伸都不会让框跑偏。 */
  function overlayDraw(seq) {
    var c = overlayCanvas;
    if (!c || !overlayCtx || !imgEl) return;
    var wrap = c.parentNode;
    if (!wrap) return;
    var wr = wrap.getBoundingClientRect();
    var cw = wr.width, ch = wr.height;
    if (!cw || !ch) return;
    var dpr = window.devicePixelRatio || 1;
    var pw = Math.round(cw * dpr), ph = Math.round(ch * dpr);
    if (c.width !== pw || c.height !== ph) {
      c.width = pw; c.height = ph;
      c.style.width = cw + 'px'; c.style.height = ch + 'px';
    }
    var ctx = overlayCtx;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cw, ch);

    var rec = detMap.get(seq);
    if (!rec) {
      /* 该 seq 没有结果（推理跳帧或结果尚未到达）：退到最近的更早一帧，
         落后超过 6 帧就不画，避免框明显脱离画面 */
      var best = null;
      detMap.forEach(function(v) {
        if (v.seq <= seq && (!best || v.seq > best.seq)) best = v;
      });
      if (!best || (seq - best.seq) > 6) return;
      rec = best;
    }
    if (!rec.d || !rec.d.length) return;

    var iw = rec.w || imgEl.naturalWidth || 1280;
    var ih = rec.h || imgEl.naturalHeight || 720;
    var ir = imgEl.getBoundingClientRect();
    var boxW = ir.width || cw, boxH = ir.height || ch;
    var fit = '';
    try { fit = (window.getComputedStyle(imgEl).objectFit || '').trim(); } catch (e) {}
    var sx, sy;
    if (fit === 'fill') {
      sx = boxW / iw; sy = boxH / ih;          /* 非等比拉伸：两轴各自换算 */
    } else {
      var s;
      if (fit === 'cover')          s = Math.max(boxW / iw, boxH / ih);
      else if (fit === 'none')      s = 1;
      else                          s = Math.min(boxW / iw, boxH / ih);   /* contain/默认 */
      if (fit === 'scale-down')     s = Math.min(1, s);
      sx = sy = s;
    }
    /* 内容左上角（wrapper 坐标系）：img 元素偏移 + letterbox 留边 */
    var ox = (ir.left - wr.left) + (boxW - iw * sx) / 2;
    var oy = (ir.top - wr.top) + (boxH - ih * sy) / 2;

    /* 线宽/字号跟随显示尺寸缩放，小窗不糊、大屏不细 */
    var ui = Math.max(0.8, Math.min(2.2, sx / 0.97));
    ctx.lineWidth = Math.round(2 * ui * 10) / 10;
    var lw = ctx.lineWidth;
    ctx.font = '500 ' + Math.round(13 * ui) + 'px system-ui,-apple-system,sans-serif';
    ctx.textBaseline = 'top';
    var labelH = Math.round(16 * ui);
    for (var i = 0; i < rec.d.length; i++) {
      var d = rec.d[i];
      var x1 = ox + d[0] * sx, y1 = oy + d[1] * sy;
      var x2 = ox + d[2] * sx, y2 = oy + d[3] * sy;
      /* cover 把画面超出容器的部分裁掉了：贴画面边缘的框，边框会落到可视区之外，
         于是只剩三条边（常见于贴着左边/上边的目标）。把框裁进可视区、预留半个线宽，
         贴边的那条边框就会画在容器边缘上，框看起来才是闭合的。 */
      var bx1 = Math.max(x1, lw / 2), by1 = Math.max(y1, lw / 2);
      var bx2 = Math.min(x2, cw - lw / 2), by2 = Math.min(y2, ch - lw / 2);
      if (bx2 <= bx1 || by2 <= by1) continue;   /* 整个框都在可视区外 */
      var col = DET_COLORS[Math.abs(d[4]) % DET_COLORS.length];
      ctx.strokeStyle = col;
      ctx.strokeRect(bx1, by1, bx2 - bx1, by2 - by1);
      var label = d[6] + ' ' + Number(d[5]).toFixed(2);
      var tw = ctx.measureText(label).width;
      /* 标签固定在标注框内部左上角：跟着框走，不会因为框贴到画面顶端就翻到框下方去。
         框比标签还矮 / 还窄时向内收，依旧留在框内。 */
      var chipW = tw + 8, chipX = bx1 + 2, chipY = by1 + 2;
      if (chipY + labelH > by2 - 1) chipY = Math.max(by1 + 1, by2 - labelH - 1);
      if (chipX + chipW > bx2 - 1) chipX = Math.max(bx1 + 1, bx2 - chipW - 1);
      /* 框本身被裁得很靠边时，标签也要整块留在可视区内，不要跟着被裁掉一半 */
      chipX = Math.max(1, Math.min(chipX, cw - chipW - 1));
      chipY = Math.max(1, Math.min(chipY, ch - labelH - 1));
      ctx.fillStyle = col;
      ctx.fillRect(chipX, chipY, chipW, labelH);
      ctx.fillStyle = '#ffffff';
      ctx.fillText(label, chipX + 4, chipY + 1);
    }
  }

  /* Retain only JPEG data and scan each incoming byte once. Blob copies the
     completed view before this reusable buffer receives the next frame.
     帧间是 multipart 头，先从里面取出 X-Seq（采集序号）和 X-Ts（采集时刻），
     再交给回调：前者把检测结果精确对齐到正在显示的这一帧，后者用来算端到端延迟。 */
  function createJpegParser(onFrame) {
    var limit = 16 * 1024 * 1024, frame = new Uint8Array(64 * 1024);
    var used = 0, previous = 0, inside = false;
    var hdrText = '', pendingSeq = 0, pendingTs = 0;
    function append(bytes) {
      var needed = used + bytes.length;
      if (needed > limit) throw new Error('MJPEG frame exceeds 16 MiB');
      if (needed > frame.length) {
        var grown = new Uint8Array(Math.min(limit, Math.max(needed, frame.length * 2)));
        grown.set(frame.subarray(0, used));
        frame = grown;
      }
      frame.set(bytes, used);
      used = needed;
    }
    /* 头里两个字段一起取：每帧头只过一次，避免重复扫描同一段文本 */
    function takeHdr() {
      var ms = /X-Seq:\s*(\d+)/g, mt = /X-Ts:\s*(\d+)/g, m, last = null, lastTs = null;
      while ((m = ms.exec(hdrText)) !== null) last = m[1];
      while ((m = mt.exec(hdrText)) !== null) lastTs = m[1];
      hdrText = '';
      pendingSeq = last !== null ? parseInt(last, 10) >>> 0 : 0;
      pendingTs = lastTs !== null ? parseInt(lastTs, 10) : 0;
    }
    return function(chunk) {
      var start = 0;
      for (var i = 0; i < chunk.length; i++) {
        var byte = chunk[i];
        if (!inside) {
          /* 只累积帧外字节（每帧约百来字节的头），超过上限截断 */
          hdrText += String.fromCharCode(byte);
          if (hdrText.length > 512) hdrText = hdrText.slice(-512);
          if (previous === 0xff && byte === 0xd8) {
            takeHdr();
            inside = true; used = 2;
            frame[0] = 0xff; frame[1] = 0xd8;
            start = i + 1;
          }
        } else if (previous === 0xff && byte === 0xd9) {
          append(chunk.subarray(start, i + 1));
          onFrame(frame.subarray(0, used), pendingSeq, pendingTs);
          inside = false; used = 0; start = i + 1;
        }
        previous = byte;
      }
      if (inside) append(chunk.subarray(start));
    };
  }

  function streamStart(url) {
    if (document.hidden || playerSuspended) return;
    if (streamUrl === url && streamRunning) return;   /* 同一流已在播放 */
    streamStop();
    streamUrl = url;
    streamRunning = true;
    resetFps();
    detReset();
    if (!overlayCanvas) {
      overlayCanvas = document.getElementById('stream_overlay');
      overlayCtx = overlayCanvas ? overlayCanvas.getContext('2d') : null;
    }
    var ac = new AbortController();
    streamAbort = ac;
    var parse = createJpegParser(function(frame, seq, ts) {
      if (!imgEl) return;
      fpsCnt++;
      /* 序号跳号 = 中间有帧没送到（服务端跳帧或链路丢弃），据此统计丢帧 */
      if (seq) {
        if (mjLastSeq && seq > mjLastSeq + 1) mjDropTotal += seq - mjLastSeq - 1;
        if (seq > mjLastSeq) mjLastSeq = seq;
      }
      var blob = new Blob([frame], {type: 'image/jpeg'});
      if (imgBusy) { pendingBlob = blob; pendingBlobSeq = seq; pendingBlobTs = ts; }
      else showFrame(blob, seq, ts);
    });
    /* 检测结果与画面同一条时间线：每 60ms 增量拉一次，覆盖 30fps 的全部 seq */
    detTimer = setInterval(detPoll, 60);

    fetch(url, {signal: ac.signal}).then(function(r) {
      if (streamAbort !== ac) return;
      if (!r.ok || !r.body) { streamRunning = false; return; }
      var rd = r.body.getReader();
      (function pump() {
        rd.read().then(function(c) {
          if (streamAbort !== ac) return;
          if (c.done) { streamRunning = false; streamAbort = null; return; }
          parse(c.value);
          pump();
        }).catch(function() {
          if (streamAbort === ac) { ac.abort(); streamRunning = false; streamAbort = null; }
        });
      })();
    }).catch(function() {
      if (streamAbort === ac) { ac.abort(); streamRunning = false; streamAbort = null; }
    });
  }

  /* Read-only diagnostics for distinguishing network delivery from rendering.
     MJPEG 透传链路没有 MSE buffered/playbackQuality，延迟取每帧 X-Ts 到上屏的真实间隔，
     丢帧用序号跳号统计（比 <video> 的 droppedVideoFrames 更贴近真实链路损失）。 */
  window.getCarVideoDiagnostics = function() {
    var vid = document.getElementById('stream_video');
    var result = {
      transport: streamRunning ? 'MJPEG' : 'idle',
      receivedFps: receivedFps, displayedFps: fpsVal, cameraFps: cameraFps,
      processingAgeMs: processingAgeMs, sourceDroppedFrames: sourceDrops,
      droppedFramesPerSecond: streamRunning ? mjDropPerSec : 0,
      latencyMs: streamRunning ? mjLatencyMs : null,
      readyState: vid ? vid.readyState : 0, currentTime: vid ? vid.currentTime : 0,
      paused: vid ? vid.paused : true, seeking: vid ? vid.seeking : false,
      decoderError: vid && vid.error ? vid.error.code : null
    };
    return result;
  };

  if (document.addEventListener)
    document.addEventListener('visibilitychange', function() {
      if (document.hidden) streamStop();
      else updatePlayer();
    });

  window.addEventListener('pagehide', function() {
    playerSuspended = true; streamStop();
  });
  window.addEventListener('pageshow', function() {
    playerSuspended = false; updatePlayer();
  });

  function barColor(p) { return p < 60 ? 'green' : p < 85 ? 'yellow' : 'red'; }

  /* 取某次请求在本机时间线上的「收到首字节」时刻。用 Resource Timing 而不是
     performance.now()：后者要等主线程排到才执行，解 1080p JPEG 时能被推迟几十毫秒，
     会把钟差算小、延迟压到 0；网络栈打点不受渲染线程负载影响。
     必须按 startTime 认出「本次」那条记录：新条目是异步写入的，直接取最后一条可能
     拿到上一次的（差整 1 秒，钟差直接崩）。取不到返回 0，本次样本作废。 */
  function resourceResponseStart(needle, afterMs) {
    if (!performance.getEntriesByType) return performance.now();
    var list = performance.getEntriesByType('resource');
    for (var i = list.length - 1; i >= 0; i--) {
      var e = list[i];
      if (e.name.indexOf(needle) >= 0 && e.startTime >= afterMs - 50 && e.responseStart > 0)
        return e.responseStart;
    }
    return 0;
  }

  async function updateVideoStats() {
    if (videoStatsBusy) return;
    videoStatsBusy = true;
    try {
      var tReq = performance.now();
      var response = await fetch('/api/video/stats');
      if (!response.ok) return;
      var stats = await response.json();
      cameraFps = stats.capture_fps || 0;
      processingAgeMs = stats.processing_age_ms || 0;
      sourceDrops = (stats.capture_dropped || 0) + (stats.driver_dropped || 0) + (stats.infer_dropped || 0);
      /* 钟差 = 板端单调时钟 - 本机时间。板端在写响应前打 now_ms，本机用「响应首字节」
         打点，两者之间只剩服务端写出+网络这一小段，误差 1~3ms 且恒为正（即估计值
         永远略小于真实钟差）。取窗口内最大值，就是最紧的那个下界 */
      var base = stats.now_ms ? resourceResponseStart('/api/video/stats', tReq) : 0;
      if (!base) base = performance.now();   /* 没有 Resource Timing 时退化；两者都是钟差下界，可混用 */
      if (stats.now_ms) {
        srvOffsets.push(stats.now_ms - base);
        if (srvOffsets.length > 8) srvOffsets.shift();
      }
    } catch(e) {} finally { videoStatsBusy = false; }
  }

  function fmtSpeed(bytesPerSec) {
    if (bytesPerSec < 0) return '--';
    if (bytesPerSec < 1024) return bytesPerSec.toFixed(0);
    if (bytesPerSec < 1024 * 1024) return (bytesPerSec / 1024).toFixed(1);
    return (bytesPerSec / 1024 / 1024).toFixed(1);
  }
  function fmtUnit(bytesPerSec) {
    if (bytesPerSec < 0) return '';
    if (bytesPerSec < 1024) return 'B/s';
    if (bytesPerSec < 1024 * 1024) return 'KB/s';
    return 'MB/s';
  }

  function initNetUI() {
    if (netInited) return;
    var h = '';
    var ifaces = [{id:'eth0',name:'有线'},{id:'wlan0',name:'无线'}];
    for (var i = 0; i < ifaces.length; i++) {
      h += '<div class="net-iface"><h4><span data-i18n="' + ifaces[i].name + '">' + t(ifaces[i].name) + '</span>'
        + '<span class="if-status" id="ns_' + ifaces[i].id + '_st">--</span></h4>';
      h += '<div class="net-row"><span class="net-dir">↓</span>'
        + '<span class="net-val" id="ns_' + ifaces[i].id + '_rx">--</span>'
        + '<span class="net-unit" id="ns_' + ifaces[i].id + '_rx_u"></span></div>';
      h += '<div class="net-row"><span class="net-dir">↑</span>'
        + '<span class="net-val" id="ns_' + ifaces[i].id + '_tx">--</span>'
        + '<span class="net-unit" id="ns_' + ifaces[i].id + '_tx_u"></span></div>';
      h += '</div>';
    }
    document.getElementById('net_speed').innerHTML = h;
    netInited = true;
  }

  async function updateNet() {
    try {
      var r = await fetch('/api/network');
      var d = await r.json();
      initNetUI();
      var now = Date.now();

      ['eth0','wlan0'].forEach(function(id) {
        var iface = d[id];
        var prev = lastNet[id];
        var active = iface && (iface.rx > 0 || iface.tx > 0);

        /* 状态 */
        var stEl = document.getElementById('ns_' + id + '_st');
        stEl.textContent = active ? t('已连接') : t('未连接');
        stEl.className = 'if-status ' + (active ? 'if-up' : 'if-down');

        if (active && prev.ts > 0) {
          var dt = (now - prev.ts) / 1000;
          if (dt > 0) {
            var rxSpeed = (iface.rx - prev.rx) / dt;
            var txSpeed = (iface.tx - prev.tx) / dt;
            document.getElementById('ns_' + id + '_rx').textContent = fmtSpeed(rxSpeed);
            document.getElementById('ns_' + id + '_rx_u').textContent = fmtUnit(rxSpeed);
            document.getElementById('ns_' + id + '_tx').textContent = fmtSpeed(txSpeed);
            document.getElementById('ns_' + id + '_tx_u').textContent = fmtUnit(txSpeed);
          }
        }

        prev.rx = iface ? iface.rx : 0;
        prev.tx = iface ? iface.tx : 0;
        prev.ts = now;
      });
    } catch(e) {}
  }

  /* withFreq：CPU 卡在每个核下面单独显示该核的实时频率；NPU 没有逐核频率，不显示 */
  var rgaCount = -1;

  function initCoreBars(id, names, withFreq) {
    var h = '';
    for (var i = 0; i < names.length; i++)
      h += '<div><div class="core-label"><span>' + names[i] + '</span><span id="' + id + '_pct' + i + '">0%</span></div>' +
           '<div class="core-bar"><div id="' + id + '_bar' + i + '" style="width:0%"></div></div>' +
           (withFreq ? '<div class="core-freq" id="' + id + '_freq' + i + '">-</div>' : '') + '</div>';
    var el = document.getElementById(id);
    el.innerHTML = h;
    /* 列数随核数自适应：核少则单排大格，核多则自动换行（尺寸随数量变化） */
    el.style.gridTemplateColumns = 'repeat(' + Math.min(names.length, 4) + ', minmax(0, 1fr))';
  }

  /* 逐核频率：后端 cpu_freqs 单位 MHz，-1/缺失表示读不到 */
  function fmtGHz(mhz) {
    return (typeof mhz === 'number' && mhz > 0) ? (mhz / 1000).toFixed(2) + ' GHz' : '-';
  }

  function fmtTime() {
    var d = new Date(), wk = ['日','一','二','三','四','五','六'];
    var y = d.getFullYear();
    var m = ('0' + (d.getMonth() + 1)).slice(-2);
    var day = ('0' + d.getDate()).slice(-2);
    var h = ('0' + d.getHours()).slice(-2);
    var min = ('0' + d.getMinutes()).slice(-2);
    return y + '_' + m + '_' + day + ' ' + t('星期' + wk[d.getDay()]) + ' ' + h + ':' + min;
  }

  async function update() {
    try {
      var r = await fetch('/api/system');
      var d = await r.json();
      /* 时钟先更新：主界面没有内存/磁盘元素，后面那段落会在 catch 里中断 */
      var clock = document.getElementById('datetime');
      if (clock) clock.textContent = fmtTime();
      document.getElementById('temp').textContent = d.temp + '°C';

      /* 总体 CPU */
      var cpuPct = 0;
      if (lastCpu.total > 0) {
        var du = d.cpu_user + d.cpu_sys - lastCpu.user - lastCpu.sys;
        var dt = d.cpu_total - lastCpu.total;
        cpuPct = dt > 0 ? Math.round(du * 100 / dt) : 0;
      }
      lastCpu = {user:d.cpu_user, sys:d.cpu_sys, idle:d.cpu_idle, total:d.cpu_total};
      /* 状态页不显示总负载：这些元素可能不存在，保持可选 */
      var valEl = document.getElementById('cpu_val');
      if (valEl) valEl.textContent = cpuPct + '%';
      var cb = document.getElementById('cpu_bar');
      if (cb) { cb.style.width = cpuPct + '%'; cb.className = barColor(cpuPct); }

      /* CPU 逐核（核数随平台自适应：真实核数来自后端 d.cpu_cores，不再写死 8 核） */
      if (d.cpu_cores) {
        var nCpu = d.cpu_cores.length;
        if (nCpu !== cpuCount) {
          cpuCount = nCpu;
          var cpuNames = [];
          for (var ci = 0; ci < nCpu; ci++) cpuNames.push('Core' + ci);
          initCoreBars('cpu_cores', cpuNames, true);
          lastCores = [];   /* 核数变化后重置差值基准 */
        }
        if (!lastCores.length) lastCores = d.cpu_cores.map(function(c) { return {u:c.u,s:c.s,i:c.i}; });
        for (var i = 0; i < nCpu; i++) {
          var c = d.cpu_cores[i], l = lastCores[i];
          var du2 = c.u + c.s - l.u - l.s, di = c.i - l.i, dt2 = du2 + di;
          var p = dt2 > 0 ? Math.round(du2 * 100 / dt2) : 0;
          document.getElementById('cpu_cores_pct' + i).textContent = p + '%';
          var b = document.getElementById('cpu_cores_bar' + i);
          b.style.width = p + '%'; b.className = barColor(p);
          /* 每个核各自的实时频率（RK3576 各核能跑在不同频点，汇总成区间会掩盖真实状态） */
          var fe = document.getElementById('cpu_cores_freq' + i);
          if (fe && d.cpu_freqs) fe.textContent = fmtGHz(d.cpu_freqs[i]);
        }
        lastCores = d.cpu_cores.map(function(c) { return {u:c.u,s:c.s,i:c.i}; });
      }

      /* NPU 逐核（核数自适应：按后端 /api/system 的 npu_count 动态渲染，
         RK3588 三核 / RK3576 双核 / RK3566 单核均自动匹配） */
      if (d.npu_count !== undefined && d.npu_count !== npuCount) {
        npuCount = d.npu_count;
        var npuNames = [];
        for (var ni = 0; ni < npuCount; ni++) npuNames.push('NPU' + ni);
        if (!npuNames.length) {
          /* 后端无法获取 NPU 负载时保留单行占位，显示 N/A */
          npuNames.push('NPU');
          initCoreBars('npu_cores', npuNames);
          document.getElementById('npu_cores_pct0').textContent = 'N/A';
        } else {
          initCoreBars('npu_cores', npuNames);
        }
      }
      if (d.npu_cores) {
        for (var i = 0; i < d.npu_cores.length; i++) {
          var p2 = d.npu_cores[i];
          var pctEl = document.getElementById('npu_cores_pct' + i);
          if (!pctEl) continue;
          pctEl.textContent = (p2 >= 0 ? p2 + '%' : 'N/A');
          var b2 = document.getElementById('npu_cores_bar' + i);
          b2.style.width = (p2 >= 0 ? p2 : 0) + '%'; b2.className = 'indigo';
        }
      }

      document.getElementById('mem_val').textContent = d.mem_pct + '%';
      document.getElementById('mem_total').textContent = (d.mem_total/1024/1024).toFixed(1) + 'G';
      document.getElementById('mem_used').textContent = ((d.mem_total - d.mem_avail)/1024/1024).toFixed(1) + 'G';
      var mb = document.getElementById('mem_bar');
      mb.style.width = d.mem_pct + '%'; mb.className = d.mem_pct < 60 ? 'green' : d.mem_pct < 85 ? 'yellow' : 'red';

      /* RGA 逐核（与 NPU 同一套模式：按后端 rga_count 动态渲染，
         无法获取时保留占位显示 N/A） */
      if (d.rga_count !== undefined && d.rga_count !== rgaCount) {
        rgaCount = d.rga_count;
        var rgaNames = [];
        for (var rn = 0; rn < rgaCount; rn++) rgaNames.push('RGA' + rn);
        if (!rgaNames.length) {
          rgaNames.push('RGA');
          initCoreBars('rga_cores', rgaNames);
          document.getElementById('rga_cores_pct0').textContent = 'N/A';
        } else {
          initCoreBars('rga_cores', rgaNames);
        }
      }
      if (d.rga_cores) {
        for (var ri = 0; ri < d.rga_cores.length; ri++) {
          var rp = d.rga_cores[ri];
          var rpEl = document.getElementById('rga_cores_pct' + ri);
          if (!rpEl) continue;
          rpEl.textContent = (rp >= 0 ? rp + '%' : 'N/A');
          var rb = document.getElementById('rga_cores_bar' + ri);
          if (rb) { rb.style.width = (rp >= 0 ? rp : 0) + '%'; rb.className = 'cyan'; }
        }
      }

      var dp = Math.round(100 - d.disk_avail * 100 / d.disk_total);
      document.getElementById('disk_val').textContent = dp + '%';
      document.getElementById('disk_total').textContent = (d.disk_total > 1024 ? (d.disk_total/1024).toFixed(1) + 'G' : d.disk_total + 'M');
      document.getElementById('disk_used').textContent = (d.disk_total - d.disk_avail > 1024 ? ((d.disk_total - d.disk_avail)/1024).toFixed(1) + 'G' : (d.disk_total - d.disk_avail) + 'M');
      var db = document.getElementById('disk_bar');
      db.style.width = dp + '%'; db.className = barColor(dp);

      document.getElementById('datetime').textContent = fmtTime();

    } catch(e) {}
    updateNet();
    if (HAS_VIDEO) updateVideoStats();
  }

  var playerConfig = {}, playerBusy = false, playerSuspended = false;
  /* 直播走相机 MJPEG 原始透传：不解码、不重编码，画面就是相机原生 JPEG。
     检测框不烧进画面，改由 canvas 按帧序号叠加（见 overlayDraw）。 */
  var LIVE_STREAM_URL = '/video/mjpeg';
  async function updatePlayer() {
    if (!HAS_VIDEO || playerBusy || document.hidden || playerSuspended) return;
    playerBusy = true;
    try {
      if (!window.STREAM_URL) {
        try {
          var response = await fetch('/api/config', {signal: AbortSignal.timeout(2500)});
          if (response.ok) playerConfig = await response.json();
        } catch(e) {}
      }
      if (document.hidden || playerSuspended) return;
      var streamUrl = window.STREAM_URL || playerConfig.stream_url || playerConfig.video_url
                      || LIVE_STREAM_URL;
      var vid = document.getElementById('stream_video');
      var img = document.getElementById('stream_img');
      var useImg = streamUrl.indexOf('/video/mjpeg') === 0
                 || /\.mjpeg$|\.jpg$|\.jpeg$/i.test(streamUrl);
      if (useImg) {
        imgEl = img;                    /* 播放器逐帧喂 blob URL，显示与计数同一条连接 */
        img.style.display = '';
        vid.style.display = 'none';
        streamStart(streamUrl);         /* 每次轮询都调用：内部幂等，流中断时自动重连 */
      } else {
        imgEl = null;
        streamStop();
        if (!vid.src || vid.getAttribute('data-src') !== streamUrl) {
          vid.setAttribute('data-src', streamUrl);
          vid.src = streamUrl;
          vid.style.display = '';
          img.style.display = 'none';
          resetFps();
        }
      }
    } finally { playerBusy = false; }
  }
  if (HAS_VIDEO) (function pollPlayer() {
    updatePlayer().catch(function() {}).then(function() { setTimeout(pollPlayer, 1000); });
  })();

  /* Do not overlap API polls while a slow response is still in flight. */
  (function poll() {
    update().then(function() { setTimeout(poll, 1000); }, function() { setTimeout(poll, 1000); });
  })();

  /* 每秒刷新视频流 FPS 徽标：窗口内无新帧则显示 --（流停滞）。
     仅主界面（有视频元素）需要；状态页没有该元素。 */
  if (HAS_VIDEO) setInterval(function() {
    var el = document.getElementById('video_fps');
    if (!el) return;
    var now = Date.now();
    if (now > fpsWin) {
      var elapsed = now - fpsWin;
      receivedFps = fpsCnt * 1000 / elapsed;
      fpsVal = streamRunning ? shownCnt * 1000 / elapsed : receivedFps;
      mjDropPerSec = mjDropMark === null ? 0
                     : Math.max(0, mjDropTotal - mjDropMark) * 1000 / elapsed;
      mjDropMark = mjDropTotal;
      fpsCnt = 0; shownCnt = 0; fpsWin = now;
    }
    el.textContent = fpsVal > 0 ? Math.round(fpsVal) + ' FPS' : '-- FPS';
    el.title = 'Displayed: ' + Math.round(fpsVal) + ' FPS; received: ' + Math.round(receivedFps) + ' FPS';
    var stats = document.getElementById('video_stats');
    if (stats) {
      var d = window.getCarVideoDiagnostics();
      stats.textContent = t('摄像头 {camera} FPS · 处理 {processing} ms · 接收 {received} FPS · 端到端延迟 {delay} ms · 当前丢帧 {dropped}/s', {
        camera: d.cameraFps ? Math.round(d.cameraFps) : '--', processing: Math.round(d.processingAgeMs),
        received: Math.round(d.receivedFps),
        delay: d.latencyMs === null ? '--' : Math.round(d.latencyMs),
        dropped: Math.round(d.droppedFramesPerSecond)
      });
    }
  }, 1000);

  /* Catch up inside the current buffer. Reopen the stream only when transport
     stops; reconnecting to another GOP for ordinary playback lag adds stalls. */

  window.reboot = function() { if (!confirm(t('确定要重启设备吗？'))) return; authFetch('/api/reboot').catch(function(){}); };
  window.shutdown = function() { if (!confirm(t('确定要关闭设备吗？'))) return; authFetch('/api/shutdown').catch(function(){}); };
})();
