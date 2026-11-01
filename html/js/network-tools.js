// SPDX-License-Identifier: GPL-3.0-or-later
(function () {
  'use strict';
  const root = document.getElementById('card_nettest');
  if (!root) return;
  /* ---- 网络测试 ---- */
  var ntLatest = {};
  function ntError(error) {
    var message = t(String(error || ''));
    if (getLang() === 'en' && /[\u3400-\u9fff]/.test(message)) message = t('网络测试失败');
    var span = document.createElement('span');
    span.textContent = message;
    return span.innerHTML;
  }
  var ntSpeedSel = document.getElementById('nt_speed_target');
  var ntSpeedCustom = document.getElementById('nt_speed_custom');
  if (ntSpeedSel) {
    ntSpeedSel.addEventListener('change', function() {
      ntSpeedCustom.style.display = this.value === 'custom' ? '' : 'none';
    });
  }

  function ntResultEl(type) {
    return document.getElementById(type === 'ping' ? 'nt_ping_result' :
                                   type === 'speed' ? 'nt_speed_result' :
                                   'nt_reach_result');
  }

  function ntTarget(type) {
    if (type === 'ping') return document.getElementById('nt_ping_target').value.trim();
    if (type === 'speed') {
      var sel = document.getElementById('nt_speed_target');
      if (sel.value === 'custom') return document.getElementById('nt_speed_custom').value.trim();
      return sel.value;
    }
    if (type === 'reach') return document.getElementById('nt_reach_target').value.trim();
    return '';
  }

  function ntRenderPing(el, data) {
    if (!data || !data.result) return;
    var r = data.result;
    if (r.reachable) {
      var html = '<span class="nt-ok">' + t('✓ 可达') + '</span>';
      if (r.packet_loss >= 0) html += ' &nbsp;' + t('丢包') + ' <span class="nt-val">' + r.packet_loss + '%</span>';
      if (r.rtt_avg >= 0) html += ' &nbsp;' + t('延迟') + ' <span class="nt-val">' + r.rtt_avg.toFixed(1) + ' ms</span>';
      if (r.rtt_min >= 0 && r.rtt_max >= 0) html += ' &nbsp;(min ' + r.rtt_min.toFixed(1) + ' / max ' + r.rtt_max.toFixed(1) + ')';
      el.innerHTML = html;
    } else {
      el.innerHTML = '<span class="nt-fail">' + t('✗ 不可达') + '</span>' + (data.error ? ' <span class="nt-fail">' + ntError(data.error) + '</span>' : '');
    }
  }

  function ntRenderSpeed(el, data) {
    if (!data || !data.result) return;
    var r = data.result;
    if (r.speed_mbps !== undefined) {
      var mbps = r.speed_mbps;
      var label = mbps >= 1 ? mbps.toFixed(2) + ' Mbps' : (mbps * 1000).toFixed(1) + ' Kbps';
      el.innerHTML = '<span class="nt-ok">' + t('✓ 下载速度') + '</span> <span class="nt-val">' + label + '</span>'
        + ' &nbsp;(' + (r.size_bytes / 1048576).toFixed(1) + ' MB / ' + r.time_sec.toFixed(1) + 's)';
    } else {
      el.innerHTML = '<span class="nt-fail">' + t('✗ 测速失败') + '</span>' + (data.error ? ' <span class="nt-fail">' + ntError(data.error) + '</span>' : '');
    }
  }

  function ntRenderReach(el, data) {
    if (!data || !data.result) return;
    var r = data.result;
    if (r.reachable) {
      var html = '<span class="nt-ok">' + t('✓ 网站可达') + '</span>';
      if (r.rtt_avg >= 0) html += ' &nbsp;' + t('延迟') + ' <span class="nt-val">' + r.rtt_avg.toFixed(1) + ' ms</span>';
      el.innerHTML = html;
    } else {
      el.innerHTML = '<span class="nt-fail">' + t('✗ 网站不可达') + '</span>' + (data.error ? ' <span class="nt-fail">' + ntError(data.error) + '</span>' : '');
    }
  }

  function ntRender(type, data) {
    ntLatest[type] = data;
    var el = ntResultEl(type);
    if (!el) return;
    if (data.status === 'starting' || data.status === 'pending' || data.status === 'running') {
      el.className = 'nt-result testing';
      el.textContent = t(data.status === 'starting' ? '⏳ 启动测试...' : '⏳ 测试中...');
      return;
    }
    el.className = 'nt-result';
    if (data.status === 'timedout' || data.status === 'failed') {
      el.innerHTML = '<span class="nt-fail">' + (data.status === 'timedout' ? t('测试超时') :
        t('请求失败') + (data.httpStatus ? ' (HTTP ' + data.httpStatus + ')' : '')) + '</span>';
      return;
    }
    if (data.error && !data.result) {
      el.innerHTML = '<span class="nt-fail">✗ ' + ntError(data.error) + '</span>';
      return;
    }
    if (type === 'ping' || type === 'reach') ntRenderPing(el, data);
    else if (type === 'speed') ntRenderSpeed(el, data);
  }

  async function ntPoll(type, testId) {
    var maxAttempts = 30;
    for (var i = 0; i < maxAttempts; i++) {
      await new Promise(function(r) { setTimeout(r, 1000); });
      try {
        var r = await fetch('/api/nettest/result?id=' + testId);
        if (!r.ok) continue;
        var data = await r.json();
        ntRender(type, data);
        if (data.status === 'done') return;
      } catch(e) {}
    }
    ntRender(type, {status: 'timedout'});
  }

  async function runNetTest(type) {
    var target = ntTarget(type);
    if (!target) {
      cardStatus('nettest', 'err', '请输入目标地址');
      return;
    }

    /* 可达性测试复用 ping 后端，从 URL 提取主机名 */
    var apiType = type;
    var apiTarget = target;
    if (type === 'reach') {
      apiType = 'ping';
      try {
        var u = new URL(target);
        apiTarget = u.hostname;
      } catch(e) {
        apiTarget = target.replace(/^https?:\/\//, '').split('/')[0];
      }
    }

    var el = ntResultEl(type);
    ntRender(type, {status: 'starting'});

    try {
      var r = await authFetch('/api/nettest', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'type=' + encodeURIComponent(apiType) + '&target=' + encodeURIComponent(apiTarget)
      });
      if (!r.ok) {
        ntRender(type, {status: 'failed', httpStatus: r.status});
        return;
      }
      var data = await r.json();
      if (data.test_id) ntPoll(type, data.test_id);
    } catch(e) {
      ntRender(type, {status: 'failed'});
    }
  };

  root.querySelectorAll('[data-net-test]').forEach(button => {
    button.addEventListener('click', () => runNetTest(button.dataset.netTest));
  });

  document.addEventListener('languagechange', function() {
    Object.keys(ntLatest).forEach(function(type) { ntRender(type, ntLatest[type]); });
  });

})();
