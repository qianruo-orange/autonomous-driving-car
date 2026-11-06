// SPDX-License-Identifier: GPL-3.0-or-later
(function () {
  'use strict';
  const root = document.getElementById('card_cpufreq');
  if (!root) return;
  const cardStatus = App.status;
  /* ---- CPU 频率配置 ---- */
  var cpuPolicies = [];
  var cpuLoading = false;
  var cpuSaving = false;
  var cpuPolicySelect = document.getElementById('cf_policy');
  function selectedCpuPolicy() {
    return cpuPolicies.find(function (policy) { return policy.id === cpuPolicySelect.value; });
  }
  function showCpuPolicy(controls) {
    var d = selectedCpuPolicy();
    if (!d) return;
    if (controls) {
      var govSel = document.getElementById('cf_gov');
      govSel.replaceChildren();
      var govNames = {performance:'性能模式',ondemand:'按需调频',schedutil:'调度器驱动',
        powersave:'省电模式',conservative:'保守模式',userspace:'用户空间'};
      (d.governors || []).forEach(function (governor) {
        var option = document.createElement('option'); option.value = governor;
        setI18nText(option, govNames[governor] || governor); govSel.appendChild(option);
      });
      govSel.value = d.requested_governor || d.governor || '';
      function fillFrequency(id, requested, actual) {
        var select = document.getElementById(id); select.replaceChildren();
        var values = (d.frequencies || []).slice();
        var current = Number(requested || actual);
        if (current > 0 && !values.includes(current)) values.push(current);
        values.sort(function (a,b) { return a-b; });
        values.forEach(function (frequency) {
          var option = document.createElement('option'); option.value = String(frequency);
          option.textContent = (frequency / 1000).toFixed(0); select.appendChild(option);
        });
        select.value = String(current);
      }
      fillFrequency('cf_min',d.requested_min_freq,d.min_freq);
      fillFrequency('cf_max',d.requested_max_freq,d.max_freq);
    }
    setI18nText(document.getElementById('cf_cur'),
      '{policy} · CPU {cpus} · 当前 {current} MHz · 实际 {min}–{max} MHz · 设置 {requestedMin}–{requestedMax} MHz', {
        policy:d.id, cpus:d.related_cpus || '—', current:(d.cur_freq/1000).toFixed(0),
        min:(d.min_freq/1000).toFixed(0), max:(d.max_freq/1000).toFixed(0),
        requestedMin:((d.requested_min_freq || d.min_freq)/1000).toFixed(0),
        requestedMax:((d.requested_max_freq || d.max_freq)/1000).toFixed(0)
      });
  }
  cpuPolicySelect.addEventListener('change',function () { showCpuPolicy(true); });
  async function loadCpuFreq(preserveEdits, signal) {
    if (cpuLoading) return;
    cpuLoading = true;
    try {
      var d = await App.json('/api/cpu_freq', {signal});
      var previous = cpuPolicySelect.value;
      cpuPolicies = d.policies || [Object.assign({id:'policy0',related_cpus:'0'},d)];
      cpuPolicySelect.replaceChildren();
      cpuPolicies.forEach(function (policy) {
        var option=document.createElement('option'); option.value=policy.id;
        option.textContent=policy.id+' · CPU '+(policy.related_cpus || '—'); cpuPolicySelect.appendChild(option);
      });
      if (cpuPolicies.some(function (policy) { return policy.id === previous; })) cpuPolicySelect.value=previous;
      showCpuPolicy(!preserveEdits || previous !== cpuPolicySelect.value);
    } catch(e) {} finally { cpuLoading = false; }
  }
  App.poll(function (signal) { if (!cpuSaving) return loadCpuFreq(true, signal); }, {interval: 5000});

  async function saveCpuFreq() {
    if (cpuSaving || !selectedCpuPolicy()) return;
    var policy = cpuPolicySelect.value;
    var gov = document.getElementById('cf_gov').value;
    var minF = document.getElementById('cf_min').value;
    var maxF = document.getElementById('cf_max').value;

    if (parseInt(minF) > parseInt(maxF)) {
      cardStatus('cpufreq', 'err', '最低频率不能大于最高频率');
      return;
    }

    try {
      cpuSaving = true;
      var r = await authFetch('/api/cpu_freq', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'policy=' + encodeURIComponent(policy) + '&governor=' + encodeURIComponent(gov) +
              '&min_freq=' + encodeURIComponent(minF) +
              '&max_freq=' + encodeURIComponent(maxF)
      });
      if (r.ok) {
        cardStatus('cpufreq', 'ok', '✓ 已保存，重启后自动恢复');
        await loadCpuFreq(false);
      } else {
        cardStatus('cpufreq', 'err', '保存失败 ({status})', {status: 'HTTP ' + r.status});
      }
    } catch(e) {
      cardStatus('cpufreq', 'err', '请求失败');
    } finally { cpuSaving = false; }
  }

  root.querySelector('[data-action="cpu-save"]').addEventListener('click', saveCpuFreq);
  App.components.register('cpu-frequency', {refresh: loadCpuFreq});
})();
