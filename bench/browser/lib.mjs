// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Shared pieces of the lab's browser tools: launching an instrumented
// Chromium, collecting what the audio tap hears, counting WebSocket
// traffic by stream, and finding the scene's latency marker in audio.
import {execFileSync, spawn} from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {chromium} from 'playwright';

const HERE = path.dirname(fileURLToPath(import.meta.url));

// The lab's PulseAudio (pulse.sh): a null sink with the system's clock.
export function labPulse() {
  if (process.env.FB_PULSE_SERVER) return process.env.FB_PULSE_SERVER;
  return execFileSync(path.join(HERE, 'pulse.sh'), ['start']).toString().trim();
}

export async function launch({width = 1920, height = 1080} = {}) {
  // The full browser in headless mode, playing into the lab's null sink.
  // The headless shell's own fake output skips ticks under load, which ran
  // its audio clock 2 to 7 percent slow here (pulse.sh says more). We run
  // as root, where Chromium's sandbox cannot start.
  const browser = await chromium.launch({
    channel: 'chromium',
    headless: true,
    chromiumSandbox: false,
    // FB_SECURE_ORIGINS lets a plain-HTTP lab address count as a secure
    // context, as the same page behind HTTPS does: AudioWorklet instead of
    // the ScriptProcessor fallback.
    args: ['--autoplay-policy=no-user-gesture-required',
           ...(process.env.FB_SECURE_ORIGINS ? [`--unsafely-treat-insecure-origin-as-secure=${process.env.FB_SECURE_ORIGINS}`] : [])],
    ignoreDefaultArgs: ['--mute-audio'],
    env: {...process.env, PULSE_SERVER: labPulse()},
  });
  // The tap's recorder is a worklet module from a blob: URL, which a strict
  // Content-Security-Policy (FernSDR's, for one) refuses. bypassCSP lets it
  // load in every page alike; what the page itself loads and runs is what
  // its own code asks for either way.
  const context = await browser.newContext({viewport: {width, height}, bypassCSP: true});
  const tap = new TapCollector();
  await context.exposeBinding('__fbTapSend', (_source, kind, payload) => tap.on(kind, payload));
  await context.addInitScript({path: path.join(HERE, 'tap.js')});
  browser.on('disconnected', () => tap.close());
  return {browser, context, tap};
}

// What the tap sends, per audio context: blocks of samples placed by their
// frame index, and the clock pairs.
export class TapCollector {
  constructor() {
    this.contexts = new Map();
    this.errors = [];
    this.media = [];
    this.samples = 0;
    this.events = 0;
    this.overflow = false;
    this.visibility = [];
    this.decoders = new Map();
  }

  on(kind, p) {
    if (this.overflow) return;
    if (++this.events > 150000 || this.samples > 24000000 || this.contexts.size > 32) {
      this.overflow = true;
      this.errors.push('tap collection limit exceeded');
      return;
    }
    if (kind === 'error') this.errors.push(p);
    if (kind === 'media') this.media.push(p);
    if (kind === 'visibility') this.visibility.push(p);
    if (kind === 'context') this.contexts.set(p.id, {info: p, blocks: [], clocks: [], rate: p.sampleRate});
    const c = this.contexts.get(p.id);
    if (!c) return;
    if (kind === 'pcm-start') {
      const process = spawn('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-probesize', '32768',
        '-analyzeduration', '0', '-f', 'matroska', '-i', 'pipe:0', '-c:a', 'pcm_f32le', '-f', 'f32le', 'pipe:1'],
        {stdio: ['pipe', 'pipe', 'pipe']});
      const decoder = {process, buffered: Buffer.alloc(0), bytes: 0, errors: ''};
      decoder.counter = new CounterDecoder(p, (frame, data) => this.on('audio',
        {id: p.id, frame, rate: p.rate, data: Buffer.from(data.buffer, data.byteOffset, data.byteLength).toString('base64')}),
        error => this.on('error', {what: 'native PCM counter', message: error}));
      process.stdout.on('data', bytes => {
        decoder.buffered = Buffer.concat([decoder.buffered, bytes]);
        const length = decoder.buffered.length - decoder.buffered.length % 8;
        if (length) decoder.counter.push(decoder.buffered.subarray(0, length));
        decoder.buffered = decoder.buffered.subarray(length);
      });
      process.stderr.on('data', bytes => { decoder.errors = (decoder.errors + bytes.toString()).slice(0, 2000); });
      process.stdin.on('error', error => this.on('error', {what: 'native PCM pipe', message: String(error)}));
      decoder.done = new Promise(resolve => {
        process.once('error', error => { this.on('error', {what: 'native PCM decoder', message: String(error)}); resolve(); });
        process.once('exit', code => {
          // A context that never recorded a byte (NovaSDR opens one it never
          // plays) leaves ffmpeg with no stream header; that is no lost audio.
          if ((code !== 0 && decoder.bytes > 0) || decoder.buffered.length)
            this.on('error', {what: 'native PCM decoder', code, message: decoder.errors});
          resolve();
        });
      });
      this.decoders.set(p.id, decoder);
    }
    if (kind === 'pcm-data') {
      const d = this.decoders.get(p.id);
      if (!d) { this.errors.push('PCM data before stream header'); return; }
      const bytes = Buffer.from(p.data, 'base64');
      d.bytes += bytes.length;
      if (d.bytes > 200000000) { this.overflow = true; this.errors.push('PCM byte budget exceeded'); this.close(); return; }
      if (!d.process.stdin.write(bytes)) return new Promise(resolve => {
        const done = () => { d.process.stdin.off('drain', done); d.process.stdin.off('error', done); resolve(); };
        d.process.stdin.once('drain', done); d.process.stdin.once('error', done);
      });
    }
    if (kind === 'pcm-end') this.decoders.get(p.id)?.process.stdin.end();
    if (kind === 'tap-kind') c.info.tapKind = p.kind;
    if (kind === 'audio') {
      const buf = Buffer.from(p.data, 'base64');
      const data = new Float32Array(buf.buffer, buf.byteOffset, buf.byteLength / 4);
      if (!Number.isSafeInteger(p.frame) || p.frame < 0 || data.length > 65536 || !Number.isFinite(p.rate) || p.rate < 8000 || p.rate > 192000) {
        this.errors.push('invalid tap block');
        return;
      }
      this.samples += data.length;
      c.blocks.push({frame: p.frame, data: Float32Array.from(data)});
      c.rate = p.rate;
    }
    if (kind === 'clock') c.clocks.push(p);
  }

  async flush(page) {
    let timer;
    try {
      await Promise.race([
        (async () => {
          await page.evaluate(() => Promise.all((window.__fbTap?.contexts ?? []).map(e => e.finish?.())));
          await Promise.all([...this.decoders.values()].map(d => d.done));
        })(),
        new Promise(resolve => { timer = setTimeout(() => {
          this.errors.push('PCM decoder flush timeout'); this.close(); resolve();
        }, 5000); }),
      ]);
    } finally { clearTimeout(timer); }
  }

  close() {
    for (const d of this.decoders.values()) if (d.process.exitCode === null) d.process.kill('SIGKILL');
  }

  // The context that played the most: some pages make several.
  loudest() {
    let best = null;
    let bestEnergy = -1;
    for (const c of this.contexts.values()) {
      let e = 0;
      for (const b of c.blocks) for (const v of b.data) e += v * v;
      if (e > bestEnergy) {
        best = c;
        bestEnergy = e;
      }
    }
    return best;
  }

  // One contiguous recording from the first block on; frames the tap never
  // delivered are NaN, so that analysis can tell a hole from silence.
  static assemble(c, window = null) {
    if (!c || !c.blocks.length) return {rate: 0, first: 0, x: new Float32Array(0), holes: 0};
    const blocks = [...c.blocks].sort((a, b) => a.frame - b.frame);
    const first = Math.max(blocks[0].frame, window?.first ?? 0);
    const last = blocks[blocks.length - 1];
    const end = Math.min(last.frame + last.data.length, window?.end ?? Infinity);
    if (end - first > c.rate * 400) throw new Error('tap frame span exceeds 400 seconds');
    const x = new Float32Array(Math.max(0, end - first)).fill(NaN);
    for (const b of blocks) {
      const from = Math.max(first, b.frame), to = Math.min(end, b.frame + b.data.length);
      if (to > from) x.set(b.data.subarray(from-b.frame, to-b.frame), from-first);
    }
    let holes = 0;
    for (const v of x) if (Number.isNaN(v)) holes++;
    return {rate: c.rate, first, x, holes};
  }
}

// The PCM counter is exact in float32: every value is an integer / 2^20.
// Missing native frames become timestamp gaps; they never compress time.
export class CounterDecoder {
  constructor({start, period, rate}, emit, error) {
    this.start = start; this.period = period; this.rate = rate;
    this.emit = emit; this.error = error; this.previous = null; this.frame = null;
  }
  push(bytes) {
    let first = null, samples = [];
    const flush = () => {
      if (samples.length) this.emit(first, Float32Array.from(samples));
      samples = []; first = null;
    };
    for (let offset = 0; offset < bytes.length; offset += 8) {
      const audio = bytes.readFloatLE(offset), counter = bytes.readFloatLE(offset+4);
      if (counter === 0 && this.previous === null) continue;
      const scaled = counter*this.period;
      const tick = Math.round(scaled)-1;
      if (!Number.isFinite(scaled) || tick < 0 || tick >= this.period || Math.abs(scaled-Math.round(scaled)) > .001) {
        this.error('PCM counter was altered or resampled'); return;
      }
      if (this.previous === null) this.frame = this.start + tick;
      else {
        const delta = (tick-this.previous+this.period)%this.period;
        if (delta === 0 || delta > this.rate) { this.error('ambiguous PCM counter'); return; }
        this.frame += delta;
        if (delta !== 1) { flush(); this.error(`PCM capture lost ${delta-1} frames`); }
      }
      this.previous = tick;
      if (first === null) first = this.frame;
      samples.push(audio);
    }
    flush();
  }
}

// How fast the context's audio clock ran against performance.now(), from
// the tap's getOutputTimestamp() pairs: 1.0 is right. A busy machine makes
// the browser miss its audio deadlines, and then the clock runs slow for
// every receiver alike; runs where it is off by more than 0.1 percent are
// not measurements of the receiver.
export function clockRatio(c, skipS = 2) {
  const pairs = (c?.clocks ?? []).filter((k) => k.contextTime !== null && k.performanceTime !== null && k.contextTime > skipS);
  if (pairs.length < 8) return null;
  const n = pairs.length;
  const xs = pairs.map((k) => k.performanceTime / 1000);
  const ys = pairs.map((k) => k.contextTime);
  const mx = xs.reduce((a, b) => a + b, 0) / n;
  const my = ys.reduce((a, b) => a + b, 0) / n;
  let sxy = 0;
  let sxx = 0;
  for (let i = 0; i < n; i++) {
    sxy += (xs[i] - mx) * (ys[i] - my);
    sxx += (xs[i] - mx) ** 2;
  }
  return sxx > 0 ? sxy / sxx : null;
}

// WebSocket bytes and frames per socket and per stream, by the adapter's
// classify(url, frame).
export function countSockets(page, classify) {
  const sockets = [];
  sockets.overflow = false;
  let events = 0;
  page.on('websocket', (ws) => {
    if (sockets.length >= 64) { sockets.overflow = true; return; }
    const s = {url: ws.url(), opened: Date.now(), closed: null, streams: {}};
    sockets.push(s);
    const add = (dir, payload) => {
      if (++events > 150000) { sockets.overflow = true; return; }
      const frame = typeof payload === 'string' ? payload : new Uint8Array(payload);
      let cls = 'other';
      try {
        cls = classify(s.url, frame) || 'other';
      } catch (e) {
        cls = 'unclassified';
      }
      const bytes = typeof payload === 'string' ? Buffer.byteLength(payload) : payload.length;
      const k = `${cls}:${dir}`;
      s.streams[k] ??= {frames: 0, bytes: 0, times: [], sizes: []};
      s.streams[k].frames++;
      s.streams[k].bytes += bytes;
      s.streams[k].times.push(Date.now());
      s.streams[k].sizes.push(bytes);
      // What the page sends, verbatim (the first 400 frames), so that a load
      // client can say the same things (bench/load/replay.mjs).
      if (dir === 'tx' && (s.sent ??= []).length < 400)
        s.sent.push({t: Date.now(), text: typeof payload === 'string' ? payload : null,
                     b64: typeof payload === 'string' ? null : Buffer.from(payload).toString('base64')});
    };
    ws.on('framereceived', (f) => add('rx', f.payload));
    ws.on('framesent', (f) => add('tx', f.payload));
    ws.on('close', () => {
      s.closed = Date.now();
    });
  });
  return sockets;
}

// The scene's marker: a maximal-length sequence keyed chip by chip, the
// same LFSR as bench/source/scene.py (degree 5, taps 5 and 3).
export function mseq(degree = 5, taps = [5, 3]) {
  let state = new Array(degree).fill(1);
  const out = [];
  for (let i = 0; i < 2 ** degree - 1; i++) {
    out.push(state[degree - 1]);
    let fb = 0;
    for (const t of taps) fb ^= state[t - 1];
    state = [fb, ...state.slice(0, -1)];
  }
  return out;
}

function fftInPlace(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      [re[i], re[j]] = [re[j], re[i]];
      [im[i], im[j]] = [im[j], im[i]];
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    for (let i = 0; i < n; i += len) {
      for (let k = 0; k < len / 2; k++) {
        const wr = Math.cos(ang * k);
        const wi = Math.sin(ang * k);
        const ur = re[i + k];
        const ui = im[i + k];
        const vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
        const vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
        re[i + k] = ur + vr;
        im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr;
        im[i + k + len / 2] = ui - vi;
      }
    }
  }
}

// The strongest tone between lo and hi Hz: a Welch spectrum (Hann windows
// of 8192 samples, half overlapping) and a parabola through the peak bin
// and its neighbours in dB. Averaging over the whole recording keeps a
// keyed tone's sidebands from winning, which a single scan did.
export function dominantTone(x, rate, lo = 300, hi = 3000) {
  const n = 8192;
  const psd = new Float64Array(n / 2);
  const win = new Float64Array(n).map((_, i) => 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / n));
  let segments = 0;
  for (let start = 0; start + n <= x.length; start += n / 2) {
    const re = new Float64Array(n);
    const im = new Float64Array(n);
    for (let i = 0; i < n; i++) {
      const v = x[start + i];
      re[i] = (Number.isNaN(v) ? 0 : v) * win[i];
    }
    fftInPlace(re, im);
    for (let k = 0; k < n / 2; k++) psd[k] += re[k] * re[k] + im[k] * im[k];
    segments++;
  }
  if (!segments) return null;
  const k0 = Math.max(1, Math.floor((lo * n) / rate));
  const k1 = Math.min(n / 2 - 2, Math.ceil((hi * n) / rate));
  let kp = k0;
  for (let k = k0; k <= k1; k++) if (psd[k] > psd[kp]) kp = k;
  const a = 10 * Math.log10(psd[kp - 1] + 1e-30);
  const b = 10 * Math.log10(psd[kp] + 1e-30);
  const c = 10 * Math.log10(psd[kp + 1] + 1e-30);
  const d = a - 2 * b + c !== 0 ? (0.5 * (a - c)) / (a - 2 * b + c) : 0;
  return ((kp + d) * rate) / n;
}

// Where the marker is in the audio: the envelope of the tone at `tone` Hz,
// correlated with one period of the chip pattern at every lag to 1 ms, in
// every period on its own. A player that plays a little fast or slow to
// hold its buffer moves the marker from period to period, so each period
// gets its own best lag. Returns, per period, the lag in seconds from the
// recording's first frame and the correlation there; `found` counts the
// periods above 0.7, and `driftMsPerPeriod` is the lag's slope.
export function findMarker(x, rate, {tone = 1000, chips = 31, chipS = 0.005, periodS = 0.5} = {}) {
  const smooth = Math.max(1, Math.round(rate * 0.002));
  const env = new Float32Array(x.length);
  const re = new Float32Array(x.length);
  const im = new Float32Array(x.length);
  for (let i = 0; i < x.length; i++) {
    const v = Number.isNaN(x[i]) ? 0 : x[i];
    re[i] = v * Math.cos((2 * Math.PI * tone * i) / rate);
    im[i] = -v * Math.sin((2 * Math.PI * tone * i) / rate);
  }
  let accRe = 0;
  let accIm = 0;
  for (let i = 0; i < x.length; i++) {
    accRe += re[i];
    accIm += im[i];
    if (i >= smooth) {
      accRe -= re[i - smooth];
      accIm -= im[i - smooth];
    }
    env[i] = Math.hypot(accRe, accIm) / smooth;
  }
  const seq = mseq(Math.round(Math.log2(chips + 1)));
  const period = Math.round(rate * periodS);
  const chip = rate * chipS;
  const stride = 4;
  const tpl = [];
  for (let i = 0; i < period; i += stride) tpl.push(seq[Math.floor(i / chip)] ?? 0);
  const tMean = tpl.reduce((a, b) => a + b, 0) / tpl.length;
  const tc = tpl.map((t) => t - tMean);
  const tVar = tc.reduce((a, t) => a + t * t, 0) / tc.length;
  const periods = Math.floor(x.length / period) - 1;
  if (periods < 1) return {lags: [], corrs: [], found: 0, periods: 0, driftMsPerPeriod: null};
  const corrAt = (base) => {
    let sx = 0;
    let sxx = 0;
    let sxt = 0;
    for (let j = 0; j < tc.length; j++) {
      const v = env[base + j * stride];
      sx += v;
      sxx += v * v;
      sxt += v * tc[j];
    }
    const n = tc.length;
    const mean = sx / n;
    const varX = sxx / n - mean * mean;
    return varX > 0 ? sxt / n / Math.sqrt(varX * tVar) : 0;
  };
  const step = Math.max(1, Math.round(rate * 0.001));
  const lags = [];
  const corrs = [];
  for (let p = 0; p < periods; p++) {
    let best = -2;
    let bestLag = 0;
    for (let lag = 0; lag < period; lag += step) {
      const c = corrAt(p * period + lag);
      if (c > best) [best, bestLag] = [c, lag];
    }
    lags.push((p * period + bestLag) / rate);
    corrs.push(best);
  }
  const found = corrs.filter((c) => c > 0.7).length;
  // Slope of marker positions against period number, less the period.
  let drift = null;
  const good = lags.map((l, i) => [i, l]).filter(([i]) => corrs[i] > 0.7);
  // A marker near a period's edge may be found in the next window or the
  // previous one; put each position where the previous one says it goes.
  for (let g = 1; g < good.length; g++) {
    const want = good[g - 1][1] + (good[g][0] - good[g - 1][0]) * periodS;
    good[g][1] += Math.round((want - good[g][1]) / periodS) * periodS;
  }
  if (good.length >= 3) {
    const m = good.length;
    const mx = good.reduce((a, [i]) => a + i, 0) / m;
    const my = good.reduce((a, [, l]) => a + l, 0) / m;
    let sxy = 0;
    let sxx = 0;
    for (const [i, l] of good) {
      sxy += (i - mx) * (l - my);
      sxx += (i - mx) ** 2;
    }
    drift = ((sxy / sxx - periodS) * 1000);
  }
  return {lags, corrs, found, periods, driftMsPerPeriod: drift};
}

export function writeWav(file, x, rate) {
  const n = x.length;
  const buf = Buffer.alloc(44 + n * 2);
  buf.write('RIFF', 0);
  buf.writeUInt32LE(36 + n * 2, 4);
  buf.write('WAVEfmt ', 8);
  buf.writeUInt32LE(16, 16);
  buf.writeUInt16LE(1, 20);
  buf.writeUInt16LE(1, 22);
  buf.writeUInt32LE(rate, 24);
  buf.writeUInt32LE(rate * 2, 28);
  buf.writeUInt16LE(2, 32);
  buf.writeUInt16LE(16, 34);
  buf.write('data', 36);
  buf.writeUInt32LE(n * 2, 40);
  for (let i = 0; i < n; i++) {
    const v = Number.isNaN(x[i]) ? 0 : Math.max(-1, Math.min(1, x[i]));
    buf.writeInt16LE(Math.round(v * 32767), 44 + i * 2);
  }
  fs.writeFileSync(file, buf);
}
