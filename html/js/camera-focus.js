// SPDX-License-Identifier: GPL-3.0-or-later
/* Shared live lens controls. Focus changes do not restart capture or inference. */
(function () {
  document.querySelectorAll('[data-camera-focus]').forEach(function (root, index) {
    var id = 'camera-focus-' + index;
    root.innerHTML = '<label class="focus-auto"><input type="checkbox" id="' + id + '-auto" disabled> <span data-i18n="自动对焦"></span></label>' +
      '<div class="focus-manual"><label for="' + id + '-position" data-i18n="手动焦距"></label>' +
      '<input type="range" id="' + id + '-position" disabled><output></output>' +
      '<button type="button" class="btn btn-back" disabled data-i18n="应用焦距"></button></div>' +
      '<p class="focus-status" role="status"></p>';
    applyI18n(root);
    var auto = root.querySelector('input[type=checkbox]');
    var slider = root.querySelector('input[type=range]');
    var output = root.querySelector('output');
    var apply = root.querySelector('button');
    var status = root.querySelector('.focus-status');
    var state = null, busy = false, loading = false, dirty = false;
    function render() {
      var a = state && state.focus_auto || {};
      var f = state && state.focus_absolute || {};
      auto.disabled = busy || !a.supported || a.read_only || a.active === false;
      if (!busy) auto.checked = !!a.value;
      slider.disabled = busy || !f.supported || f.read_only || auto.checked;
      if (f.supported) {
        slider.min = f.min; slider.max = f.max; slider.step = f.step || 1;
        if (!dirty) slider.value = f.value;
        output.textContent = slider.value;
      } else output.textContent = '—';
      apply.disabled = slider.disabled || !dirty;
    }
    async function refresh() {
      if (loading || busy || dirty || document.hidden) return;
      loading = true;
      try {
        var r = await fetch('/api/camera', {cache: 'no-store'});
        if (!r.ok) throw new Error('HTTP ' + r.status);
        state = await r.json(); render();
        setI18nText(status, state.focus_auto.supported || state.focus_absolute.supported ? '' : '此相机不支持调焦');
      } catch (e) {
        state = null; render(); setI18nText(status, '相机调焦不可用');
      } finally { loading = false; }
    }
    async function write(values) {
      if (busy) return;
      busy = true; render(); setI18nText(status, '正在应用焦距…');
      try {
        var r = await authFetch('/api/camera/focus', {method: 'POST', body: new URLSearchParams(values)});
        if (!r.ok) throw new Error('HTTP ' + r.status);
        state = await r.json(); dirty = false;
        setI18nText(status, '焦距已更新');
      } catch (e) { setI18nText(status, '调焦失败，请重试'); }
      finally { busy = false; render(); }
    }
    auto.addEventListener('change', function () {
      var enabled = auto.checked;
      // Keep the user's choice visible until the device confirms or rejects it.
      write({auto: enabled ? '1' : '0'});
    });
    slider.addEventListener('input', function () { dirty = true; output.textContent = slider.value; apply.disabled = false; });
    apply.addEventListener('click', function () {
      var values = {focus: slider.value};
      if (state && state.focus_auto.supported) values.auto = '0';
      write(values);
    });
    refresh();
    var timer = setInterval(refresh, 2500);
    window.addEventListener('pagehide', function () { clearInterval(timer); });
    // Returning from the dedicated camera page may select a different device.
    document.addEventListener('camerachange', function () { dirty = false; refresh(); });
  });
  window.toggleGatewayFullscreen = async function () {
    var wrapper = document.querySelector('.video-wrapper');
    try {
      if (document.fullscreenElement) await document.exitFullscreen();
      else if (wrapper && wrapper.requestFullscreen) await wrapper.requestFullscreen();
    } catch (e) { /* Fullscreen can be denied by browser or display policy. */ }
  };
})();
