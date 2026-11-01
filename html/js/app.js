// SPDX-License-Identifier: GPL-3.0-or-later
/* Shared page infrastructure. Feature modules own their DOM and state. */
(function () {
  'use strict';
  const components = new Map();
  const statusTimers = new WeakMap();

  async function json(url, options = {}) {
    const {authenticated = false, timeout = 5000, signal, ...request} = options;
    const controller = new AbortController();
    const abort = () => controller.abort();
    if (signal) {
      if (signal.aborted) abort();
      else signal.addEventListener('abort', abort, {once: true});
    }
    const timer = setTimeout(abort, timeout);
    try {
      const transport = authenticated ? window.authFetch : window.fetch;
      const response = await transport.call(window, url, {...request, cache: 'no-store', signal: controller.signal});
      if (!response.ok) throw Object.assign(new Error('HTTP ' + response.status), {status: response.status});
      return await response.json();
    } finally {
      clearTimeout(timer);
      if (signal) signal.removeEventListener('abort', abort);
    }
  }

  // Keep Blob URLs alive until the browser has started the download.
  function downloadBlob(blob, filename) {
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement('a');
    anchor.href = url;
    anchor.download = filename;
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  // Await completion before scheduling again. Page transitions abort active reads.
  function poll(task, {interval = 1000} = {}) {
    let timer, controller, running = false, stopped = false, paused = false;
    function schedule(delay) {
      clearTimeout(timer);
      if (!stopped && !paused && !document.hidden) timer = setTimeout(run, delay);
    }
    async function run() {
      if (running || stopped || paused || document.hidden) return;
      running = true;
      controller = new AbortController();
      try { await task(controller.signal); }
      catch (error) { if (error.name !== 'AbortError') console.warn('Polling failed:', error); }
      finally { running = false; controller = null; schedule(interval); }
    }
    function visibility() {
      if (document.hidden) { clearTimeout(timer); if (controller) controller.abort(); }
      else schedule(0);
    }
    function hide() { paused = true; clearTimeout(timer); if (controller) controller.abort(); }
    function show() { paused = false; schedule(0); }
    document.addEventListener('visibilitychange', visibility);
    window.addEventListener('pagehide', hide);
    window.addEventListener('pageshow', show);
    schedule(0);
    return function stop() {
      stopped = true;
      hide();
      document.removeEventListener('visibilitychange', visibility);
      window.removeEventListener('pagehide', hide);
      window.removeEventListener('pageshow', show);
    };
  }

  function status(cardId, type, message, values) {
    const element = document.getElementById('status_' + cardId);
    if (!element) return;
    clearTimeout(statusTimers.get(element));
    element.className = 'card-status ' + type;
    setI18nText(element, message, values);
    statusTimers.set(element, setTimeout(() => {
      element.className = 'card-status';
      setI18nText(element, '');
    }, type === 'ok' ? 3000 : 5000));
  }

  window.App = Object.freeze({json, poll, status, downloadBlob,
    components: Object.freeze({
      register(name, api) {
        if (components.has(name)) throw new Error('Duplicate component: ' + name);
        components.set(name, Object.freeze(api));
        document.dispatchEvent(new CustomEvent('app:componentchange', {detail: {name}}));
      },
      get(name) { return components.get(name); }
    })
  });
})();
