// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Injected into every page before its own scripts run. It hears what the
// page plays, without changing it: whatever a page connects to an
// AudioContext's destination is also connected to a recorder, whose blocks
// go to the harness with the context time of their first sample. Pages
// that play through an <audio> or <video> element instead are tapped
// through captureStream when they start playing.
//
// Timing. Every quarter second the tap also sends the context's
// getOutputTimestamp(), which pairs a context time with the performance
// time at which that sample is estimated to leave the output device, and
// performance.timeOrigin, which puts performance time on the wall clock.
// The harness maps each block to wall time with the pair nearest to it.
//
// The recorder runs in an AudioWorklet, on the audio thread, so a busy page
// cannot make the recording skip what the listener heard. Its module loads
// asynchronously; connections made before it is ready wait in a queue.
(() => {
  if (window.__fbTap) return;
  const send = (kind, payload) => {
    try {
      return window.__fbTapSend(kind, payload);
    } catch (e) {
      // The harness binding is not there (the page opened elsewhere).
    }
  };

  const processor = `
    class FbRecorder extends AudioWorkletProcessor {
      constructor() {
        super();
        this.buf = new Float32Array(4096);
        this.fill = 0;
        this.start = -1;
      }
      process(inputs) {
        const input = inputs[0];
        const n = input.length ? input[0].length : 128;
        if (this.start < 0) this.start = currentFrame;
        for (let i = 0; i < n; i++) {
          let v = 0;
          for (let c = 0; c < input.length; c++) v += input[c][i];
          this.buf[this.fill++] = input.length ? v / input.length : 0;
          if (this.fill === this.buf.length) {
            this.port.postMessage({frame: this.start, rate: sampleRate, data: this.buf});
            this.buf = new Float32Array(4096);
            this.fill = 0;
            this.start = currentFrame + i + 1;
          }
        }
        return true;
      }
    }
    registerProcessor('fb-recorder', FbRecorder);
  `;
  const moduleUrl = URL.createObjectURL(new Blob([processor], {type: 'application/javascript'}));

  const contexts = [];
  let nextId = 0;
  const documentId = `${performance.timeOrigin}-${Math.random().toString(36).slice(2)}`;

  function b64(f32) {
    const bytes = new Uint8Array(f32.buffer, f32.byteOffset, f32.byteLength);
    let s = '';
    for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(s);
  }

  function prepare(ctx, options) {
    const entry = {ctx, id: `${documentId}-${nextId++}`, tap: null, queue: new Map(), outputs: new WeakMap()};
    contexts.push(entry);
    send('context', {
      id: entry.id,
      sampleRate: ctx.sampleRate,
      latencyHint: options && options.latencyHint !== undefined ? String(options.latencyHint) : null,
      baseLatency: ctx.baseLatency ?? null,
    });
    const activate = (tap) => {
      entry.tap = tap;
      for (const [src, outputs] of entry.queue)
        for (const output of outputs) origConnect.call(src, tap, output, 0);
      entry.queue.clear();
    };
    if (!ctx.audioWorklet) {
      // Native PCM recording avoids ScriptProcessor's shared-buffer lock,
      // which can drop render quanta even when audible playback is intact.
      // A second, inaudible channel carries the exact context sample index.
      if (!MediaRecorder.isTypeSupported('audio/webm;codecs=pcm')) {
        send('error', {what: 'HTTP tap', message: 'native lossless PCM recorder unavailable'});
        return entry;
      }
      const period = 1 << 20;
      const buffer = ctx.createBuffer(1, period, ctx.sampleRate);
      const counter = buffer.getChannelData(0);
      for (let i = 0; i < period; i++) counter[i] = (i + 1) / period;
      const clock = ctx.createBufferSource();
      clock.buffer = buffer;
      clock.loop = true;
      const merger = ctx.createChannelMerger(2);
      const input = ctx.createGain();
      input.channelCount = 1;
      input.channelCountMode = 'explicit';
      const destination = ctx.createMediaStreamDestination();
      origConnect.call(input, merger, 0, 0);
      origConnect.call(clock, merger, 0, 1);
      origConnect.call(merger, destination);
      const start = Math.ceil(ctx.currentTime * ctx.sampleRate) + 512;
      send('pcm-start', {id: entry.id, rate: ctx.sampleRate, start, period});
      const recorder = new MediaRecorder(destination.stream, {mimeType: 'audio/webm;codecs=pcm'});
      let queued = Promise.resolve();
      let finish;
      const finished = new Promise(resolve => { finish = resolve; });
      recorder.ondataavailable = event => {
        queued = queued.then(async () => {
          const bytes = new Uint8Array(await event.data.arrayBuffer());
          if (bytes.length) await send('pcm-data', {id: entry.id, data: b64(bytes)});
        }).catch(error => send('error', {what: 'HTTP PCM capture', message: String(error)}));
      };
      recorder.onstop = () => queued.then(() => send('pcm-end', {id: entry.id})).finally(finish);
      recorder.onerror = event => send('error', {what: 'HTTP PCM recorder', message: String(event.error)});
      recorder.start(125);
      clock.start(start / ctx.sampleRate);
      entry.finish = () => {
        if (recorder.state !== 'inactive') recorder.stop();
        return finished;
      };
      ctx.addEventListener('statechange', () => {
        if (ctx.state === 'closed') entry.finish();
      });
      entry.counter = clock;
      entry.recorder = recorder;
      activate(input);
      send('tap-kind', {id: entry.id, kind: 'native-pcm-counter-v1'});
      return entry;
    }
    ctx.audioWorklet.addModule(moduleUrl).then(() => {
      const tap = new AudioWorkletNode(ctx, 'fb-recorder', {numberOfInputs: 1, numberOfOutputs: 1, channelCount: 2, channelCountMode: 'explicit'});
      tap.port.onmessage = (e) => send('audio', {id: entry.id, frame: e.data.frame, rate: e.data.rate, data: b64(e.data.data)});
      // A node must be pulled by the destination to run; through a
      // silent gain it adds nothing to what is played.
      const mute = new GainNode(ctx, {gain: 0});
      origConnect.call(tap, mute);
      origConnect.call(mute, ctx.destination);
      activate(tap);
      send('tap-kind', {id: entry.id, kind: 'audio-worklet'});
    }).catch((e) => send('error', {what: 'tap module', message: String(e)}));
    return entry;
  }

  function connectTap(entry, src, output = 0) {
    const outputs = entry.outputs.get(src) || new Set();
    if (outputs.has(output)) return;
    outputs.add(output);
    entry.outputs.set(src, outputs);
    if (entry.tap) origConnect.call(src, entry.tap, output, 0);
    else entry.queue.set(src, outputs);
  }

  const Native = window.AudioContext || window.webkitAudioContext;
  const origConnect = AudioNode.prototype.connect;
  const origDisconnect = AudioNode.prototype.disconnect;
  const byCtx = new WeakMap();

  // Reflect.construct keeps subclasses of AudioContext working.
  function Wrapped(options) {
    const ctx = Reflect.construct(Native, options === undefined ? [] : [options], new.target || Wrapped);
    byCtx.set(ctx, prepare(ctx, options));
    return ctx;
  }
  Wrapped.prototype = Native.prototype;
  Object.setPrototypeOf(Wrapped, Native);
  window.AudioContext = Wrapped;
  if (window.webkitAudioContext) window.webkitAudioContext = Wrapped;

  AudioNode.prototype.connect = function (dest, ...rest) {
    const result = origConnect.call(this, dest, ...rest);
    if (dest instanceof AudioDestinationNode) {
      const entry = byCtx.get(this.context);
      if (entry && this !== entry.tap) {
        connectTap(entry, this, rest[0] ?? 0);
      }
    }
    return result;
  };

  AudioNode.prototype.disconnect = function (...args) {
    // Apply the page's operation first so invalid overloads still throw.
    // A no-argument or output-only disconnect already removes tap edges;
    // destination-specific disconnects need their mirrors removed too.
    const result = origDisconnect.apply(this, args);
    const entry = byCtx.get(this.context);
    const outputs = entry?.outputs.get(this);
    if (!outputs) return result;
    const all = args.length === 0;
    const byOutput = typeof args[0] === 'number';
    const destination = args[0] === this.context.destination;
    if (!all && !byOutput && !destination) return result;
    for (const output of [...outputs]) {
      if (byOutput && output !== args[0]) continue;
      if (destination && args[1] !== undefined && output !== args[1]) continue;
      if (destination && entry.tap) origDisconnect.call(this, entry.tap, output, 0);
      outputs.delete(output);
    }
    if (!outputs.size) entry.queue.delete(this);
    return result;
  };

  // Media elements: tap their stream into a context of our own.
  let mediaCtx = null;
  const mediaTapped = new WeakSet();
  document.addEventListener('play', (ev) => {
    const el = ev.target;
    if (!(el instanceof HTMLMediaElement) || mediaTapped.has(el) || !el.captureStream) return;
    mediaTapped.add(el);
    if (!mediaCtx) mediaCtx = new Wrapped({});
    const entry = byCtx.get(mediaCtx);
    const src = new MediaStreamAudioSourceNode(mediaCtx, {mediaStream: el.captureStream()});
    connectTap(entry, src);
    send('media', {id: entry.id, tag: el.tagName});
  }, true);

  setInterval(() => {
    for (const entry of contexts) {
      const ctx = entry.ctx;
      const ts = ctx.getOutputTimestamp ? ctx.getOutputTimestamp() : {};
      send('clock', {
        id: entry.id,
        state: ctx.state,
        contextTime: ts.contextTime ?? null,
        performanceTime: ts.performanceTime ?? null,
        currentTime: ctx.currentTime,
        outputLatency: ctx.outputLatency ?? null,
        baseLatency: ctx.baseLatency ?? null,
        timeOrigin: performance.timeOrigin,
        now: performance.now(),
      });
    }
  }, 250);

  window.__fbTap = {contexts};
})();
