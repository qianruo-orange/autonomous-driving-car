// SPDX-License-Identifier: GPL-3.0-or-later
/* Dedicated capture settings. Focus controls are shared with the dashboard. */
(function () {
  'use strict';
  var form = document.getElementById('camera_form');
  var device = document.getElementById('camera_device');
  var codec = document.getElementById('camera_codec');
  var decoder = document.getElementById('camera_decoder');
  var mode = document.getElementById('camera_mode');
  var save = document.getElementById('camera_save');
  var status = document.getElementById('camera_status');
  var sizes = [];
  var generation = 0;
  var busy = false;
  var hardwareAvailable = null;
  var currentFps = 30;

  function message(key, error, values) {
    status.className = 'card-status' + (error ? ' err' : '');
    setI18nText(status, key, values);
  }
  function option(select, value, label) {
    var item = document.createElement('option');
    item.value = String(value);
    item.textContent = label;
    select.appendChild(item);
    return item;
  }
  function updateDisabled() {
    device.disabled = codec.disabled = decoder.disabled = mode.disabled = busy;
    save.disabled = busy || !mode.value ||
      (codec.value === 'hardware' && hardwareAvailable !== true);
  }
  function setBusy(value) { busy = value; updateDisabled(); }
  async function getJson(url) {
    var response = await fetch(url, {cache: 'no-store'});
    if (!response.ok) throw new Error('HTTP ' + response.status);
    return response.json();
  }

  /* Capture always prefers MJPEG; use that format's rates when it is available.
     A YUYV-only rate must not appear as an MJPEG capture option. */
  function intersectCaps(camera, encoder) {
    var formats = camera.formats || [];
    var format = formats.find(function (item) { return item.fmt === 'MJPG'; }) ||
      formats.find(function (item) { return item.fmt === 'YUYV'; });
    var result = [];
    ((format && format.sizes) || []).forEach(function (capture) {
      var encoded = (encoder.caps || []).find(function (item) {
        return Number(item.w) === Number(capture.w) && Number(item.h) === Number(capture.h);
      });
      if (!encoded) return;
      var rates = (capture.fps || []).map(Number).filter(function (rate, index, all) {
        return Number.isFinite(rate) && rate > 0 && all.indexOf(rate) === index &&
          (encoded.fps || []).map(Number).indexOf(rate) >= 0;
      }).sort(function (a, b) { return b - a; });
      if (rates.length) result.push({w: Number(capture.w), h: Number(capture.h), fps: rates});
    });
    return result;
  }
  async function loadCapabilities(preferredSize, preferredFps) {
    var version = ++generation;
    setBusy(true);
    mode.replaceChildren();
    try {
      if (!/^\/dev\/video\d+$/.test(device.value)) throw new Error('No capture device');
      var responses = await Promise.all([
        getJson('/api/video/caps?device=' + device.value),
        getJson('/api/video/codeccaps?codec=' + encodeURIComponent(codec.value))
      ]);
      if (version !== generation) return;
      sizes = intersectCaps(responses[0], responses[1]);
      sizes.forEach(function (item) {
        item.fps.forEach(function (rate) {
          option(mode, item.w + 'x' + item.h + '@' + rate, item.w + ' × ' + item.h + ' · ' + rate);
        });
      });
      var choices = Array.from(mode.options).map(function (item) { return item.value; });
      var preferred = preferredSize + '@' + preferredFps;
      mode.value = choices.indexOf(preferred) >= 0 ? preferred :
        choices.find(function (item) { return item.indexOf(preferredSize + '@') === 0; }) ||
        choices.find(function (item) { return item.indexOf('1280x720@') === 0; }) || choices[0] || '';
      message(sizes.length ? '' : '相机与编码器没有共同支持的采集模式', !sizes.length);
    } catch (error) {
      if (version === generation) { sizes = []; message('无法读取相机能力，请重试', true); }
    } finally {
      if (version === generation) setBusy(false);
    }
  }
  async function loadConfig() {
    if (busy) return;
    setBusy(true);
    message('正在读取相机配置…');
    try {
      var responses = await Promise.all([
        getJson('/api/config'), getJson('/api/video/devices'),
        getJson('/api/camera').catch(function () { return null; })
      ]);
      var cfg = responses[0];
      var devices = responses[1];
      device.replaceChildren();
      devices.forEach(function (item) { option(device, item.path, item.card + ' (' + item.path + ')'); });
      if (cfg.video_device && !devices.some(function (item) { return item.path === cfg.video_device; }))
        option(device, cfg.video_device, cfg.video_device);
      device.value = cfg.video_device || (devices[0] && devices[0].path) || '';
      hardwareAvailable = responses[2] && typeof responses[2].hardware_encoder_available === 'boolean' ?
        responses[2].hardware_encoder_available : null;
      codec.querySelector('option[value="hardware"]').disabled = hardwareAvailable !== true;
      setI18nText(codec.querySelector('option[value="hardware"]'), hardwareAvailable === true ? '硬件编码' : '硬件编码（不可用）');
      codec.value = cfg.video_codec === 'hardware' ? 'hardware' : 'software';
      var decoderAvailable = responses[2] && responses[2].hardware_decoder_available === true;
      decoder.querySelector('option[value="hardware"]').disabled = !decoderAvailable;
      decoder.value = cfg.video_decoder === 'hardware' && decoderAvailable ? 'hardware' : 'software';
      currentFps = Number(cfg.video_fps) || 30;
      await loadCapabilities(Number(cfg.video_width) + 'x' + Number(cfg.video_height), currentFps);
    } catch (error) { message('无法读取相机配置，请重试', true); }
    finally { setBusy(false); }
  }
  function modeChanged() {
    var parts = mode.value.split('@');
    loadCapabilities(parts[0] || '1280x720', Number(parts[1]) || currentFps);
  }
  device.addEventListener('change', modeChanged);
  codec.addEventListener('change', modeChanged);
  form.addEventListener('submit', async function (event) {
    event.preventDefault();
    if (save.disabled) return;
    var parts = mode.value.split('@');
    var dimensions = parts[0].split('x');
    setBusy(true);
    message('正在保存相机配置…');
    try {
      var response = await authFetch('/api/config', {method: 'POST', body: new URLSearchParams({
        target: 'video', video_device: device.value, video_codec: codec.value,
        video_decoder: decoder.value, video_width: dimensions[0], video_height: dimensions[1], video_fps: parts[1]
      })});
      if (!response.ok) { message('请求失败 ({status})', true, {status: response.status}); return; }
      currentFps = Number(parts[1]);
      message('✓ 已保存');
      document.dispatchEvent(new CustomEvent('camerachange'));
    } catch (error) { message('请求失败', true); }
    finally { setBusy(false); }
  });
  window.loadCameraConfig = loadConfig;
  loadConfig();
})();
