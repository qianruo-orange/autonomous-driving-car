// Source-extracted, offline regression tests. No browser/network/source edits.
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const assert = require('node:assert/strict');
const sourcePath = process.argv[2] || path.resolve(__dirname, '../html/js/monitor.js');
const source = fs.readFileSync(sourcePath, 'utf8');
const prefix = source.slice(0, source.indexOf('  function barColor('));
assert(prefix.includes('async function mseAppend('));
const exposure = `
  globalThis.api = {
    mseStart, mseStop, mseAppend, mseUpdate, mseCatchUp, mseFail,
    streamStart, streamStop,
    setImage: function(value) { imgEl = value; },
    state: function() { return { mseRunning, mseGeneration, mseCancelUpdate,
      mseSb, mseMs, mseReader, mseAbort, mseLastTrim, mseLastFail, mseDiag,
      streamRunning, streamAbort, fpsCnt, shownCnt, imgBusy, pendingBlob }; }
  };
})();`;

function deferred() {
  let resolve, reject;
  const promise = new Promise((r, j) => { resolve = r; reject = j; });
  return { promise, resolve, reject };
}
async function flush() { for (let i = 0; i < 16; i++) await Promise.resolve(); }
class Target {
  constructor() { this.events = new Map(); }
  addEventListener(type, fn) {
    if (!this.events.has(type)) this.events.set(type, new Set());
    this.events.get(type).add(fn);
  }
  removeEventListener(type, fn) { this.events.get(type)?.delete(fn); }
  emit(type) { for (const fn of [...(this.events.get(type) || [])]) fn({ type }); }
  count(type) { return this.events.get(type)?.size || 0; }
}
class Reader {
  constructor() { this.queue = []; this.waiter = null; this.readCalls = 0; this.cancelled = 0; }
  read() {
    this.readCalls++;
    if (this.queue.length) return Promise.resolve(this.queue.shift());
    assert.equal(this.waiter, null, 'Concurrent reader.read calls');
    this.waiter = deferred();
    return this.waiter.promise;
  }
  push(chunk) {
    const result = chunk === null ? { done: true } : { done: false, value: chunk };
    if (this.waiter) { const waiter = this.waiter; this.waiter = null; waiter.resolve(result); }
    else this.queue.push(result);
  }
  reject(error) {
    if (this.waiter) { const waiter = this.waiter; this.waiter = null; waiter.reject(error); }
  }
  cancel() { this.cancelled++; this.reject(new Error('Reader cancelled')); return Promise.resolve(); }
}
function makeHarness() {
  const timers = new Map();
  let now = 10000, timerId = 0;
  const sources = [], requests = [], revoked = [];
  const video = {
    src: '', currentTime: 0, paused: false, seeking: false, readyState: 3,
    loadCalls: 0, playCalls: 0,
    load() { this.loadCalls++; this.currentTime = 0; },
    play() { this.playCalls++; return Promise.resolve(); },
    removeAttribute(name) { if (name === 'src') this.src = ''; }
  };
  class Buffer extends Target {
    constructor() { super(); this.updating = false; this.operations = []; this.ranges = []; }
    get buffered() {
      const ranges = this.ranges;
      return { length: ranges.length, start: i => ranges[i][0], end: i => ranges[i][1] };
    }
    appendBuffer(chunk) {
      assert.equal(this.updating, false, 'Append while SourceBuffer is updating');
      this.updating = true; this.operations.push(['append', chunk]);
    }
    remove(start, end) {
      assert.equal(this.updating, false, 'Remove while SourceBuffer is updating');
      this.updating = true; this.operations.push(['remove', start, end]);
    }
    complete() {
      assert.equal(this.updating, true);
      const operation = this.operations.at(-1);
      if (operation[0] === 'remove') this.ranges = this.ranges.map(([a, b]) => [Math.max(a, operation[2]), b]).filter(([a, b]) => b > a);
      this.updating = false; this.emit('updateend');
    }
    abort() { this.updating = false; this.emit('abort'); this.emit('updateend'); }
  }
  class MediaSource extends Target {
    constructor() { super(); this.readyState = 'closed'; this.buffer = null; sources.push(this); }
    static isTypeSupported() { return true; }
    addSourceBuffer() { this.buffer = new Buffer(); return this.buffer; }
    removeSourceBuffer(buffer) { if (buffer.updating) buffer.abort(); this.buffer = null; }
    open() { this.readyState = 'open'; this.emit('sourceopen'); }
  }
  const document = { hidden: false, getElementById(id) { return id === 'stream_video' ? video : null; } };
  const context = {
    window: {}, document, MediaSource, AbortController, Blob, Uint8Array,
    Date: { now: () => now },
    URL: { createObjectURL: () => 'blob:mock-' + (sources.length + requests.length), revokeObjectURL: url => revoked.push(url) },
    setTimeout(callback, delay) { const id = ++timerId; timers.set(id, { at: now + delay, callback }); return id; },
    clearTimeout(id) { timers.delete(id); },
    fetch(url, options) { const request = deferred(); requests.push({ url, options, ...request }); return request.promise; }
  };
  context.window.MediaSource = MediaSource;
  vm.createContext(context);
  vm.runInContext(prefix + exposure, context, { filename: sourcePath });
  return {
    api: context.api, video, document, sources, requests, revoked, timers, Buffer,
    async advance(ms) {
      const target = now + ms;
      while (true) {
        const next = [...timers].filter(([, timer]) => timer.at <= target).sort((a, b) => a[1].at - b[1].at)[0];
        if (!next) break;
        now = next[1].at; timers.delete(next[0]); next[1].callback(); await flush();
      }
      now = target; await flush();
    },
    async start(chunks = []) {
      const reader = new Reader(); chunks.forEach(chunk => reader.push(chunk));
      const requestCount = requests.length;
      context.api.mseStart('/video/stream?test=' + requestCount);
      assert.equal(requests.length, requestCount + 1);
      requests.at(-1).resolve({ ok: true, headers: { get: () => null }, body: { getReader: () => reader } });
      await flush();
      const media = sources.at(-1); media.open(); await flush();
      return { reader, media, buffer: media.buffer, generation: context.api.state().mseGeneration };
    }
  };
}
const cases = [];
function test(name, fn) { cases.push([name, fn]); }
test('MSE pump awaits updateend and preserves delayed multi-chunk FIFO', async () => {
  const h = makeHarness(); const A = new Uint8Array([1]), B = new Uint8Array([2]), C = new Uint8Array([3]);
  const s = await h.start([A, B, C]);
  assert.equal(s.reader.readCalls, 1); assert.equal(s.buffer.operations[0][1], A);
  await h.advance(4999);
  assert.equal(s.reader.readCalls, 1, 'Next network read must wait for append completion');
  s.buffer.complete(); await flush();
  assert.equal(s.reader.readCalls, 2); assert.equal(s.buffer.operations[1][1], B);
  s.buffer.complete(); await flush();
  assert.equal(s.reader.readCalls, 3); assert.equal(s.buffer.operations[2][1], C);
  s.buffer.complete(); await flush();
  assert.equal(s.reader.readCalls, 4);
  assert.deepEqual(s.buffer.operations.map(operation => operation[1][0]), [1, 2, 3]);
  h.api.mseStop(); await flush(); assert.equal(h.timers.size, 0);
});
test('same-session stop cancels pending update and cleans operation listeners/timer', async () => {
  const h = makeHarness(); const s = await h.start([new Uint8Array([1])]);
  assert.equal(s.buffer.count('updateend'), 1); assert.equal(h.timers.size, 1);
  h.api.mseStop(); await flush();
  assert.equal(s.buffer.count('updateend'), 0); assert.equal(s.buffer.count('abort'), 0);
  assert.equal(s.buffer.count('error'), 1, 'Only the permanent generation-guarded error handler remains');
  assert.equal(h.timers.size, 0); assert.equal(h.api.state().mseCancelUpdate, null);
  assert.equal(s.reader.cancelled, 1); assert.equal(h.api.state().mseRunning, false);
});
test('old abort/read rejection and old error cannot stop replacement generation', async () => {
  const h = makeHarness(); const old = await h.start();
  const replacement = await h.start([new Uint8Array([9])]); await flush();
  const generation = replacement.generation;
  assert.equal(old.reader.cancelled, 1);
  old.buffer.emit('error'); old.buffer.emit('abort'); old.buffer.emit('updateend'); await flush();
  assert.equal(h.api.state().mseRunning, true); assert.equal(h.api.state().mseGeneration, generation);
  assert.equal(h.api.state().mseSb, replacement.buffer); assert.equal(h.api.state().mseDiag.reconnects, 0);
  replacement.buffer.complete(); await flush(); h.api.mseStop(); await flush();
});
test('old pending append rejection cannot stop replacement generation', async () => {
  const h = makeHarness(); const old = await h.start([new Uint8Array([1])]);
  const replacement = await h.start([new Uint8Array([2])]); await flush();
  assert.equal(old.buffer.count('updateend'), 0);
  assert.equal(h.api.state().mseRunning, true); assert.equal(h.api.state().mseSb, replacement.buffer);
  assert.equal(h.api.state().mseDiag.reconnects, 0);
  replacement.buffer.complete(); await flush(); h.api.mseStop(); await flush();
});
test('late response from stale fetch cannot replace new MediaSource', async () => {
  const h = makeHarness(); h.api.mseStart('/old'); const pending = h.requests.at(-1);
  const current = await h.start();
  pending.resolve({ ok: true, headers: { get: () => null }, body: { getReader() { throw new Error('Stale response must be ignored'); } } });
  await flush(); assert.equal(h.sources.length, 1); assert.equal(h.api.state().mseMs, current.media);
  h.api.mseStop(); await flush();
});
test('buffer trim completes before next append and clears old media', async () => {
  const h = makeHarness(); const s = await h.start();
  s.buffer.ranges = [[0, 40]]; h.video.currentTime = 30; await h.advance(6000);
  s.reader.push(new Uint8Array([4])); await flush();
  assert.deepEqual(s.buffer.operations, [['remove', 0, 20]]);
  assert.equal(s.reader.readCalls, 1); s.buffer.complete(); await flush();
  assert.equal(s.buffer.operations.length, 2); assert.equal(s.buffer.operations[1][0], 'append');
  assert.equal(s.buffer.buffered.start(0), 20); s.buffer.complete(); await flush();
  h.api.mseStop(); await flush();
});
test('catch-up seeks near live edge without restarting or disconnecting', async () => {
  const h = makeHarness(); const s = await h.start(); s.buffer.ranges = [[5, 10]]; h.video.currentTime = 6;
  const generation = s.generation, loads = h.video.loadCalls;
  h.api.mseCatchUp(); assert.equal(h.video.currentTime, 9.65);
  assert.equal(h.api.state().mseGeneration, generation); assert.equal(h.video.loadCalls, loads);
  assert.equal(s.reader.cancelled, 0); assert.equal(h.api.state().mseDiag.catchups, 1);
  h.api.mseStop(); await flush();
});
test('catch-up respects hidden, paused, seeking, and existing media gaps', async () => {
  const h = makeHarness(); const s = await h.start(); s.buffer.ranges = [[20, 20.2]];
  for (const [target, property] of [[h.document, 'hidden'], [h.video, 'paused'], [h.video, 'seeking']]) {
    h.video.currentTime = 10; target[property] = true; h.api.mseCatchUp();
    assert.equal(h.video.currentTime, 10); target[property] = false;
  }
  h.api.mseCatchUp(); assert.equal(h.video.currentTime, 20, 'Seek stays within final buffered range');
  h.api.mseStop(); await flush();
});
test('mseStop while MSE inactive preserves external native video source', async () => {
  const h = makeHarness(); h.video.src = 'http://example.invalid/movie.mp4';
  h.api.mseStop(); h.api.mseStop();
  assert.equal(h.video.src, 'http://example.invalid/movie.mp4'); assert.equal(h.video.loadCalls, 0);
});
test('SourceBuffer abort rejects update, removes listeners, and cancels timeout', async () => {
  const h = makeHarness(); const s = await h.start();
  const promise = h.api.mseUpdate(s.buffer, () => { s.buffer.updating = true; }, s.generation);
  const rejected = assert.rejects(promise, /Media buffer aborted/); s.buffer.abort(); await rejected;
  assert.equal(h.timers.size, 0); assert.equal(s.buffer.count('updateend'), 0);
  assert.equal(s.buffer.count('abort'), 0); assert.equal(h.api.state().mseCancelUpdate, null);
  h.api.mseStop(); await flush();
});
test('append timeout triggers exactly one current-generation reconnect', async () => {
  const h = makeHarness(); const s = await h.start([new Uint8Array([1])]); await h.advance(5000);
  assert.equal(h.api.state().mseRunning, false); assert.equal(h.api.state().mseDiag.reconnects, 1);
  assert.match(h.api.state().mseDiag.lastError, /timed out/); assert.equal(h.timers.size, 0);
  assert.equal(s.reader.cancelled, 1);
});
test('MJPEG frame larger than 64 KiB survives repeated network chunk boundaries', async () => {
  const h = makeHarness(); const image = { src: '', onload: null, onerror: null }; h.api.setImage(image);
  const reader = new Reader(); h.api.streamStart('/video/mjpeg_ai');
  h.requests.at(-1).resolve({ ok: true, body: { getReader: () => reader } }); await flush();
  const frame = new Uint8Array(100000); frame[0] = 0xff; frame[1] = 0xd8; frame[frame.length - 2] = 0xff; frame[frame.length - 1] = 0xd9;
  for (let offset = 0; offset < frame.length; offset += 32768) { reader.push(frame.subarray(offset, offset + 32768)); await flush(); }
  assert.equal(h.api.state().fpsCnt, 1); assert.match(image.src, /^blob:/);
  image.onload({ type: 'load' }); assert.equal(h.api.state().shownCnt, 1);
  h.api.streamStop(); await flush();
});

(async () => {
  for (const [name, run] of cases) {
    await run(); console.log('PASS ' + name);
  }
  console.log(`${cases.length} offline source-extracted player regression tests passed.`);
})().catch(error => { console.error(error); process.exitCode = 1; });
