// SPDX-License-Identifier: GPL-3.0-or-later
/* monitor.js — 仪表盘数据刷新 */
(function() {
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
  var lastPresented = null, receivedFps = 0;
  var lastDropped = null, droppedPerSecond = 0;
  var cameraFps = 0, processingAgeMs = 0, sourceDrops = 0, videoStatsBusy = false;
  var streamUrl = '', streamAbort = null, streamRunning = false;
  var imgEl = null, lastBlobUrl = null;
  var imgBusy = false;      /* 上一帧还在解码：不覆盖 src（覆盖会取消其加载） */
  var pendingBlob = null;   /* 解码期间收到的最新帧，仅保留最后一张，解码完立即补显 */

  function resetFps() {
    fpsCnt = 0; shownCnt = 0; fpsWin = Date.now(); fpsVal = 0;
    lastPresented = null; receivedFps = 0;
    lastDropped = null; droppedPerSecond = 0;
  }

  /* 显示一帧：仅在上一次 onload/onerror 之后调用，上一帧 URL 此时已被完整消费，
     可安全回收；onload 前就 revoke 会中断在途解码，弱设备上永远看不到画面 */
  function showFrame(blob) {
    var target = imgEl, session = streamAbort;
    if (!target || !session) return;
    var u = URL.createObjectURL(blob);
    imgBusy = true;
    target.onload = target.onerror = function(event) {
      if (streamAbort !== session || imgEl !== target) return;
      if (event.type === 'load') shownCnt++;
      imgBusy = false;
      if (pendingBlob) {          /* 解码慢于码流时丢弃中间帧，只补最新一帧 */
        var pb = pendingBlob;
        pendingBlob = null;
        showFrame(pb);
      }
    };
    target.src = u;
    if (lastBlobUrl) URL.revokeObjectURL(lastBlobUrl);
    lastBlobUrl = u;
  }

  function streamStop() {
    if (streamAbort) { streamAbort.abort(); streamAbort = null; }
    streamRunning = false;
    imgBusy = false;
    pendingBlob = null;
    if (lastBlobUrl) { URL.revokeObjectURL(lastBlobUrl); lastBlobUrl = null; }
  }

  function streamStart(url) {
    if (streamUrl === url && streamRunning) return;   /* 同一流已在播放 */
    streamStop();
    streamUrl = url;
    streamRunning = true;
    resetFps();
    var ac = new AbortController();
    streamAbort = ac;
    var buf = new Uint8Array(0);

    fetch(url, {signal: ac.signal}).then(function(r) {
      if (streamAbort !== ac) return;
      if (!r.ok || !r.body) { streamRunning = false; return; }
      var rd = r.body.getReader();
      (function pump() {
        rd.read().then(function(c) {
          if (streamAbort !== ac) return;
          if (c.done) { streamRunning = false; streamAbort = null; return; }
          var b = c.value;
          var nb = new Uint8Array(buf.length + b.length);
          nb.set(buf, 0); nb.set(b, buf.length);
          buf = nb;
          /* 扫 SOI(FF D8)…EOI(FF D9) 成帧（multipart 头被自然跳过；跨块帧头尾由缓冲续接） */
          var p = 0;
          while (p < buf.length - 1) {
            if (buf[p] === 0xFF && buf[p + 1] === 0xD8) {
              var q = p + 2;
              while (q < buf.length - 1 && !(buf[q] === 0xFF && buf[q + 1] === 0xD9)) q++;
              if (q >= buf.length - 1) break;          /* 帧未收全：等下一块 */
              if (imgEl) {
                fpsCnt++;                              /* 计数含被丢弃的帧（真实码流速率） */
                var blob = new Blob([buf.subarray(p, q + 2)], {type: 'image/jpeg'});
                if (imgBusy) pendingBlob = blob;       /* 解码忙：只留最新帧，防 src 覆盖风暴 */
                else showFrame(blob);
              }
              p = q + 2;
            } else p++;
          }
          /* 丢弃已消费数据，仅保留未完成帧的尾部 */
          if (p > 0) buf = buf.subarray(p);
          /* A partial JPEG may legitimately exceed 64 KiB. Keep its SOI until
             EOI arrives; discard a corrupt/unbounded frame by reconnecting. */
          else if (buf.length > 16 * 1024 * 1024) throw new Error('MJPEG frame exceeds 16 MiB');
          pump();
        }).catch(function() {
          if (streamAbort === ac) { ac.abort(); streamRunning = false; streamAbort = null; }
        });
      })();
    }).catch(function() {
      if (streamAbort === ac) { ac.abort(); streamRunning = false; streamAbort = null; }
    });
  }

  /* ---- H.264 fMP4 / MSE 播放器（默认 /video/stream） ----
     单条 fetch 喂 MediaSource：init 段(ftyp/moov) + 逐片段(moof/mdat) append；
     'moof' 签名计数喂 FPS 徽标（近似，仅显示用）。失败 1s 节流重连，
     服务端断流（会话轮转）立即重连；1s 看门狗防缓冲堆积与假死 */
  var mseMs = null, mseSb = null, mseAbort = null, mseReader = null;
  var mseRunning = false, mseUrl = '', mseLastData = 0, mseLastFail = 0;
  var mseTail = new Uint8Array(3);   /* 跨块 'moof' 签名续接尾巴 */
  var mseGeneration = 0, mseCancelUpdate = null, mseLastTrim = 0;
  var msePrimed = false, mseClockAgeMs = null, mseClockAt = 0;
  var chromePlayback = typeof navigator !== 'undefined' && /(?:Chrome|Chromium)\//.test(navigator.userAgent || '');
  var mseTargetLead = chromePlayback ? 0.08 : 0.2;
  var mseMaxLead = chromePlayback ? 0.25 : 0.7;
  var mseOverLeadSince = null;
  var mseFrameCallback = null, msePresentedMediaTime = null, mseDecodeMs = null;
  var mseDiag = { starts: 0, reconnects: 0, catchups: 0, appendErrors: 0, lastError: '' };

  function mseSupported() {
    if (!window.MediaSource || !MediaSource.isTypeSupported) return false;
    var codes = ['avc1.64001f', 'avc1.4d401f', 'avc1.42e01f'];
    for (var i = 0; i < codes.length; i++)
      if (MediaSource.isTypeSupported('video/mp4; codecs="' + codes[i] + '"')) return true;
    return false;
  }

  function mseStop() {
    var hadMse = mseRunning || mseMs || mseUrl;
    var vid = document.getElementById('stream_video');
    if (mseFrameCallback !== null && vid && vid.cancelVideoFrameCallback)
      vid.cancelVideoFrameCallback(mseFrameCallback);
    mseFrameCallback = null;
    msePresentedMediaTime = null;
    mseOverLeadSince = null;
    /* Invalidate callbacks before aborting the old session. Its rejected read
       must never stop the replacement stream. */
    mseGeneration++;
    mseRunning = false;
    if (mseCancelUpdate) mseCancelUpdate();
    if (mseReader) {
      try { var cancelled = mseReader.cancel(); if (cancelled) cancelled.catch(function() {}); } catch(e) {}
      mseReader = null;
    }
    if (mseAbort) { mseAbort.abort(); mseAbort = null; }
    if (mseMs) {
      try {
        if (mseSb && mseMs.readyState === 'open') mseMs.removeSourceBuffer(mseSb);
      } catch(e) {}
      mseMs = null;
    }
    mseSb = null;
    mseUrl = '';
    mseTail[0] = mseTail[1] = mseTail[2] = 0;
    if (vid && hadMse) {
      vid.playbackRate = 1;
      if (vid.src && vid.src.indexOf('blob:') === 0) URL.revokeObjectURL(vid.src);
      vid.removeAttribute('src');
      vid.load();                          /* 清空播放器（readyState 归零） */
    }
  }

  function mseFail(generation, error) {
    if (generation !== mseGeneration || !mseRunning) return;
    mseDiag.lastError = error && error.message ? error.message : String(error);
    mseDiag.reconnects++;
    mseLastFail = Date.now();
    mseStop();
  }

  /* One mutation at a time, completed by updateend. The fetch pump awaits this
     promise, so network chunks cannot overtake earlier bytes in the MP4 stream. */
  function mseUpdate(sb, action, generation) {
    return new Promise(function(resolve, reject) {
      if (!mseRunning || mseSb !== sb || generation !== mseGeneration) {
        reject(new Error('Video session ended')); return;
      }
      function finish(error) {
        clearTimeout(timer);
        sb.removeEventListener('updateend', complete);
        sb.removeEventListener('error', failed);
        sb.removeEventListener('abort', aborted);
        if (mseCancelUpdate === cancel) mseCancelUpdate = null;
        if (error) reject(error); else resolve();
      }
      function complete() { finish(); }
      function failed() { mseDiag.appendErrors++; finish(new Error('Media buffer error')); }
      function aborted() { finish(new Error('Media buffer aborted')); }
      function cancel() { finish(new Error('Video session ended')); }
      var timer = setTimeout(function() { finish(new Error('Media buffer update timed out')); }, 5000);
      mseCancelUpdate = cancel;
      sb.addEventListener('updateend', complete);
      sb.addEventListener('error', failed);
      sb.addEventListener('abort', aborted);
      try { action(); } catch(e) { mseDiag.appendErrors++; finish(e); }
    });
  }

  async function mseAppend(sb, chunk, generation) {
    var vid = document.getElementById('stream_video');
    /* Evict already played media on the same serial pipeline, preventing an
       ever-growing buffer and eventual QuotaExceeded reconnects. */
    if (Date.now() - mseLastTrim > 5000 && sb.buffered.length &&
        vid.currentTime - sb.buffered.start(0) > 20) {
      var removeEnd = vid.currentTime - 10;
      await mseUpdate(sb, function() { sb.remove(0, removeEnd); }, generation);
      mseLastTrim = Date.now();
    }
    await mseUpdate(sb, function() { sb.appendBuffer(chunk); }, generation);
    if (generation === mseGeneration) mseCatchUp();
  }

  function mseCatchUp() {
    if (!mseRunning || !mseSb) return;
    var vid = document.getElementById('stream_video');
    if (!vid || document.hidden || vid.paused || vid.seeking) {
      mseOverLeadSince = null;
      if (vid) vid.playbackRate = 1;
      return;
    }
    try {
      var b = mseSb.buffered;
      if (!b.length) { mseOverLeadSince = null; return; }
      var end = b.end(b.length - 1), start = b.start(b.length - 1);
      var ahead = end - vid.currentTime;
      var now = Date.now();
      if (chromePlayback && ahead > mseMaxLead) {
        if (mseOverLeadSince === null) mseOverLeadSince = now;
      } else mseOverLeadSince = null;
      /* Prime close to the live edge after enough media arrives. A small,
         bounded buffer absorbs frame jitter without retaining a historical GOP.
         Chrome receives frame batches on Wi-Fi: seeking on each 120 ms burst
         discarded valid frames. Drain brief bursts at a gentle rate; seek only
         after sustained backlog, or immediately for a large stale buffer. */
      var primeSpan = chromePlayback ? 0.12 : 0.2;
      var overdue = chromePlayback ? ahead > 0.75 ||
        (mseOverLeadSince !== null && now - mseOverLeadSince >= 400) : ahead > mseMaxLead;
      if ((!msePrimed && end - start >= primeSpan && ahead > mseTargetLead + 0.03) ||
          overdue || vid.currentTime < start) {
        vid.currentTime = Math.max(start, end - mseTargetLead);
        mseDiag.catchups++;
        msePrimed = true;
        mseOverLeadSince = null;
        vid.playbackRate = 1;
      } else if (ahead > mseTargetLead + (chromePlayback ? 0.04 : 0.1)) {
        vid.playbackRate = chromePlayback && ahead <= mseMaxLead ? 1.03 : 1.05;
      } else if (ahead < mseTargetLead + 0.02) {
        vid.playbackRate = 1;
      }
    } catch(e) {}
  }

  function mseScanMoof(chunk) {
    var buf = new Uint8Array(mseTail.length + chunk.length);
    buf.set(mseTail, 0);
    buf.set(chunk, mseTail.length);
    for (var i = 0; i + 3 < buf.length; i++) {
      if (buf[i] === 0x6d && buf[i+1] === 0x6f && buf[i+2] === 0x6f && buf[i+3] === 0x66) {
        fpsCnt++;
        i += 3;
      }
    }
    mseTail.set(buf.subarray(buf.length - 3));
  }

  function mseStart(url) {
    if (mseUrl === url && mseRunning) return;        /* 同一流已在播放 */
    if (Date.now() - mseLastFail < 1000) return;     /* 失败 1s 节流（update 200ms 轮询） */
    mseStop();
    mseUrl = url;
    mseRunning = true;
    mseLastData = Date.now();
    mseLastTrim = Date.now();
    msePrimed = false;
    mseOverLeadSince = null;
    mseClockAgeMs = null;
    mseClockAt = 0;
    msePresentedMediaTime = null;
    mseDecodeMs = null;
    mseDiag.starts++;
    var generation = mseGeneration;
    resetFps();
    var ac = new AbortController();
    mseAbort = ac;

    fetch(url, {signal: ac.signal}).then(function(r) {
      if (generation !== mseGeneration) return;
      if (!r.ok || !r.body) {                        /* 503（无编码会话）：节流重试 */
        mseFail(generation, new Error('Video HTTP ' + r.status));
        return;
      }
      var codec = r.headers.get('X-Codec') || 'avc1.4d401f';
      var ageHeader = r.headers.get('X-Stream-Age-Ms');
      var age = ageHeader == null ? NaN : Number(ageHeader);
      if (isFinite(age) && age >= 0 && age <= 30000) {
        mseClockAgeMs = age;
        mseClockAt = Date.now();
      }
      var ms = new MediaSource();
      mseMs = ms;
      var vid = document.getElementById('stream_video');
      vid.src = URL.createObjectURL(ms);
      var rd = r.body.getReader();
      mseReader = rd;

      ms.addEventListener('sourceopen', function() {
        if (mseMs !== ms || generation !== mseGeneration) return;
        var sb;
        try {
          sb = ms.addSourceBuffer('video/mp4; codecs="' + codec + '"');
        } catch(e) {
          mseFail(generation, e);
          return;
        }
        mseSb = sb;
        mseTrackPresentedFrames(vid, generation);
        sb.addEventListener('error', function() { mseFail(generation, new Error('Media decoding failed')); });
        var p = vid.play();
        if (p && p.catch) p.catch(function(){});     /* muted+autoplay 正常应成功 */

        (async function pump() {
          try {
            while (generation === mseGeneration && mseRunning) {
              var c = await rd.read();
              if (generation !== mseGeneration) return;
              if (c.done) { mseStop(); return; }
              mseLastData = Date.now();
              mseScanMoof(c.value);
              await mseAppend(sb, c.value, generation);
            }
          } catch(e) { mseFail(generation, e); }
        })();
      });
    }).catch(function(e) { mseFail(generation, e); });
  }

  function mseTrackPresentedFrames(vid, generation) {
    if (!vid.requestVideoFrameCallback) return;
    function presented(now, metadata) {
      if (!mseRunning || generation !== mseGeneration) return;
      msePresentedMediaTime = metadata.mediaTime;
      if (typeof metadata.processingDuration === 'number') mseDecodeMs = metadata.processingDuration * 1000;
      mseFrameCallback = vid.requestVideoFrameCallback(presented);
    }
    mseFrameCallback = vid.requestVideoFrameCallback(presented);
  }

  /* Read-only diagnostics for distinguishing network delivery from rendering. */
  window.getGatewayVideoDiagnostics = function() {
    var vid = document.getElementById('stream_video');
    var result = Object.assign({}, mseDiag, {
      transport: mseRunning ? 'H.264/MSE' : streamRunning ? 'MJPEG' : 'idle',
      receivedFps: receivedFps, displayedFps: fpsVal, bufferAheadSeconds: 0,
      droppedFramesPerSecond: droppedPerSecond, cameraFps: cameraFps,
      processingAgeMs: processingAgeMs, sourceDroppedFrames: sourceDrops,
      estimatedStreamDelayMs: null, buffered: [],
      readyState: vid ? vid.readyState : 0, currentTime: vid ? vid.currentTime : 0,
      paused: vid ? vid.paused : true, seeking: vid ? vid.seeking : false,
      playbackRate: vid ? vid.playbackRate : 1,
      playbackProfile: chromePlayback ? 'Chrome low latency' : 'Compatible',
      targetBufferMs: mseTargetLead * 1000, browserDecodeMs: mseDecodeMs,
      decoderError: vid && vid.error ? vid.error.code : null
    });
    if (vid && vid.getVideoPlaybackQuality) {
      var q = vid.getVideoPlaybackQuality();
      result.totalVideoFrames = q.totalVideoFrames;
      result.droppedVideoFrames = q.droppedVideoFrames;
    }
    if (vid && mseSb && mseSb.buffered.length) {
      result.bufferAheadSeconds = Math.max(0, mseSb.buffered.end(mseSb.buffered.length - 1) - vid.currentTime);
      for (var i = 0; i < mseSb.buffered.length; i++)
        result.buffered.push([mseSb.buffered.start(i), mseSb.buffered.end(i)]);
    }
    if (mseRunning && vid && mseClockAgeMs !== null) {
      var mediaTime = msePresentedMediaTime === null ? vid.currentTime : msePresentedMediaTime;
      result.estimatedStreamDelayMs = Math.max(0, mseClockAgeMs + Date.now() - mseClockAt - mediaTime * 1000);
    }
    result.estimatedTotalDelayMs = result.estimatedStreamDelayMs === null ? null : result.estimatedStreamDelayMs + processingAgeMs;
    return result;
  };

  function mseWatchdog() {
    if (!mseRunning) return;
    if (document.hidden) { mseCatchUp(); return; }
    /* Include connection setup: a stalled fetch/sourceopen has no buffer yet. */
    if (Date.now() - mseLastData > 5000) {
      mseFail(mseGeneration, new Error('No video data for 5 seconds'));
      return;
    }
    if (mseSb && mseMs) mseCatchUp();
  }

  var playerVideo = document.getElementById('stream_video');
  if (playerVideo && playerVideo.addEventListener) {
    playerVideo.addEventListener('error', function() {
      if (mseRunning && playerVideo.error)
        mseFail(mseGeneration, new Error('Video decoder error ' + playerVideo.error.code));
    });
    playerVideo.addEventListener('seeked', function() { mseCatchUp(); });
    function resetCatchUp() {
      mseOverLeadSince = null;
      if (mseRunning) playerVideo.playbackRate = 1;
    }
    playerVideo.addEventListener('pause', resetCatchUp);
    playerVideo.addEventListener('seeking', resetCatchUp);
    if (document.addEventListener)
      document.addEventListener('visibilitychange', function() {
        if (document.hidden) resetCatchUp();
      });
  }

  function barColor(p) { return p < 60 ? 'green' : p < 85 ? 'yellow' : 'red'; }

  async function updateVideoStats() {
    if (videoStatsBusy) return;
    videoStatsBusy = true;
    try {
      var response = await fetch('/api/video/stats');
      if (!response.ok) return;
      var stats = await response.json();
      cameraFps = stats.capture_fps || 0;
      processingAgeMs = stats.capture_to_encode_ms || stats.processing_age_ms || 0;
      sourceDrops = (stats.capture_dropped || 0) + (stats.driver_dropped || 0) + (stats.infer_dropped || 0);
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

  function initCoreBars(id, names) {
    var h = '';
    for (var i = 0; i < names.length; i++)
      h += '<div><div class="core-label"><span>' + names[i] + '</span><span id="' + id + '_pct' + i + '">0%</span></div><div class="core-bar"><div id="' + id + '_bar' + i + '" style="width:0%"></div></div></div>';
    var el = document.getElementById(id);
    el.innerHTML = h;
    /* 列数随核数自适应：核少则单排大格，核多则自动换行（尺寸随数量变化） */
    el.style.gridTemplateColumns = 'repeat(' + Math.min(names.length, 4) + ', minmax(0, 1fr))';
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
      document.getElementById('temp').textContent = d.temp + '°C';

      /* 总体 CPU */
      var cpuPct = 0;
      if (lastCpu.total > 0) {
        var du = d.cpu_user + d.cpu_sys - lastCpu.user - lastCpu.sys;
        var dt = d.cpu_total - lastCpu.total;
        cpuPct = dt > 0 ? Math.round(du * 100 / dt) : 0;
      }
      lastCpu = {user:d.cpu_user, sys:d.cpu_sys, idle:d.cpu_idle, total:d.cpu_total};
      document.getElementById('cpu_val').textContent = cpuPct + '%';
      var cb = document.getElementById('cpu_bar');
      cb.style.width = cpuPct + '%'; cb.className = barColor(cpuPct);

      /* CPU 逐核（核数随平台自适应：真实核数来自后端 d.cpu_cores，不再写死 8 核） */
      if (d.cpu_cores) {
        var nCpu = d.cpu_cores.length;
        if (nCpu !== cpuCount) {
          cpuCount = nCpu;
          var cpuNames = [];
          for (var ci = 0; ci < nCpu; ci++) cpuNames.push('Core' + ci);
          initCoreBars('cpu_cores', cpuNames);
          lastCores = [];   /* 核数变化后重置差值基准 */
        }
        if (!lastCores.length) lastCores = d.cpu_cores.map(function(c) { return {u:c.u,s:c.s,i:c.i}; });
        for (var i = 0; i < nCpu; i++) {
          var c = d.cpu_cores[i], l = lastCores[i];
          var du2 = c.u + c.s - l.u - l.s, di = c.i - l.i, dt2 = du2 + di;
          var p = dt2 > 0 ? Math.round(du2 * 100 / dt2) : 0;
          var freqText = (d.cpu_freqs && d.cpu_freqs[i]) ? ' <small>' + (d.cpu_freqs[i] / 1000).toFixed(2) + 'GHz</small>' : '';
          document.getElementById('cpu_cores_pct' + i).innerHTML = p + '%' + freqText;
          var b = document.getElementById('cpu_cores_bar' + i);
          b.style.width = p + '%'; b.className = barColor(p);
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

      /* GPU：优先显示负载%；RK356x/RK3576 驱动未暴露 load 时显示当前频率(MHz) */
      var gpuEl = document.getElementById('gpu_val');
      var gb = document.getElementById('gpu_bar');
      var gf = document.getElementById('gpu_freq');
      if (d.gpu >= 0) {
        gpuEl.textContent = d.gpu + '%';
        gb.style.width = d.gpu + '%'; gb.className = 'blue';
        if (gf) gf.textContent = '';
      } else if (d.gpu_freq >= 0) {
        gpuEl.textContent = d.gpu_freq + ' MHz';
        gb.style.width = '0%';
        if (gf) gf.textContent = '';
      } else {
        gpuEl.textContent = 'N/A';
        gb.style.width = '0%';
        if (gf) gf.textContent = '';
      }

      var dp = Math.round(100 - d.disk_avail * 100 / d.disk_total);
      document.getElementById('disk_val').textContent = dp + '%';
      document.getElementById('disk_total').textContent = (d.disk_total > 1024 ? (d.disk_total/1024).toFixed(1) + 'G' : d.disk_total + 'M');
      document.getElementById('disk_used').textContent = (d.disk_total - d.disk_avail > 1024 ? ((d.disk_total - d.disk_avail)/1024).toFixed(1) + 'G' : (d.disk_total - d.disk_avail) + 'M');
      var db = document.getElementById('disk_bar');
      db.style.width = dp + '%'; db.className = barColor(dp);

      document.getElementById('datetime').textContent = fmtTime();
      /* 视频流：默认走 H.264 fMP4/MSE（~2Mbps，弱网不卡顿）；
         显式配置 URL 或浏览器不支持 MSE 时回退原 img/video 管线 */
      try {
        var streamUrl = window.STREAM_URL || '';
        var explicit = !!streamUrl;
        if (!explicit) {
          var r2 = await fetch('/api/config');
          if (r2.ok) {
            var cfg = await r2.json();
            if (cfg.stream_url || cfg.video_url) {
              streamUrl = cfg.stream_url || cfg.video_url;
              explicit = true;
            }
          }
        }
        var vid = document.getElementById('stream_video');
        var img = document.getElementById('stream_img');
        var useMse = !explicit && mseSupported();
        if (!explicit && !useMse) streamUrl = '/video/mjpeg_ai';   /* 无 MSE：回退画框流 */
        if (useMse) {
          /* MSE 路径：录制编码扇出（无会话 503 节流重试，会话轮转断流自动重连） */
          imgEl = null;
          streamStop();
          img.style.display = 'none';
          vid.style.display = '';
          mseStart('/video/stream');        /* 每次轮询都调用：内部幂等 + 失败节流重连 */
        } else if (streamUrl) {
          mseStop();
          var useImg = streamUrl.indexOf('/video/mjpeg') === 0
                     || streamUrl.match(/\.mjpeg$|\.jpg$|\.jpeg$/i);
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
        }
      } catch(e) {}
    } catch(e) {}
    updateNet();
    updateVideoStats();
  }

  /* Do not overlap API polls while a slow response is still in flight. */
  (function poll() {
    update().then(function() { setTimeout(poll, 1000); }, function() { setTimeout(poll, 1000); });
  })();

  /* 每秒刷新视频流 FPS 徽标：窗口内无新帧则显示 --（流停滞） */
  setInterval(function() {
    var el = document.getElementById('video_fps');
    if (!el) return;
    var now = Date.now();
    if (now > fpsWin) {
      var elapsed = now - fpsWin;
      receivedFps = fpsCnt * 1000 / elapsed;
      var vid = document.getElementById('stream_video');
      if (mseRunning && vid && vid.getVideoPlaybackQuality) {
        var quality = vid.getVideoPlaybackQuality();
        var presented = quality.totalVideoFrames - quality.droppedVideoFrames;
        fpsVal = lastPresented === null ? 0 : Math.max(0, presented - lastPresented) * 1000 / elapsed;
        lastPresented = presented;
        droppedPerSecond = lastDropped === null ? 0 : Math.max(0, quality.droppedVideoFrames - lastDropped) * 1000 / elapsed;
        lastDropped = quality.droppedVideoFrames;
      } else {
        fpsVal = streamRunning ? shownCnt * 1000 / elapsed : receivedFps;
      }
      fpsCnt = 0; shownCnt = 0; fpsWin = now;
    }
    el.textContent = fpsVal > 0 ? Math.round(fpsVal) + ' FPS' : '-- FPS';
    el.title = 'Displayed: ' + Math.round(fpsVal) + ' FPS; received: ' + Math.round(receivedFps) + ' FPS';
    var stats = document.getElementById('video_stats');
    if (stats) {
      var d = window.getGatewayVideoDiagnostics();
      stats.textContent = t('摄像头 {camera} FPS · 处理 {processing} ms · 接收 {received} FPS · 缓冲 {buffer} ms · 总延迟约 {delay} ms · 当前丢帧 {dropped}/s', {
        camera: d.cameraFps ? Math.round(d.cameraFps) : '--', processing: Math.round(d.processingAgeMs),
        received: Math.round(d.receivedFps), buffer: Math.round(d.bufferAheadSeconds * 1000),
        delay: d.estimatedTotalDelayMs === null ? '--' : Math.round(d.estimatedTotalDelayMs),
        dropped: Math.round(d.droppedFramesPerSecond)
      });
    }
  }, 1000);

  /* Catch up inside the current buffer. Reopen the stream only when transport
     stops; reconnecting to another GOP for ordinary playback lag adds stalls. */
  setInterval(mseWatchdog, 1000);

  window.reboot = function() { if (!confirm(t('确定要重启设备吗？'))) return; authFetch('/api/reboot').catch(function(){}); };
  window.shutdown = function() { if (!confirm(t('确定要关闭设备吗？'))) return; authFetch('/api/shutdown').catch(function(){}); };
})();

window.toggleGatewayFullscreen = async function () {
  try {
    if (document.fullscreenElement) await document.exitFullscreen();
    else { var view = document.querySelector(".video-wrapper"); if (view && view.requestFullscreen) await view.requestFullscreen(); }
  } catch (error) {}
};
