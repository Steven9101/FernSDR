import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { writeFile } from 'node:fs/promises';
import { chromium } from 'playwright';

const url = new URL(process.argv[2] ?? 'http://127.0.0.1:8073/');
const kbps = Number(process.argv[3] ?? 64);
const seconds = Number(process.argv[4] ?? 45);
const outageAt = Number(process.argv[5] ?? 10);
const outageMs = Number(process.argv[6] ?? 0);
const jitterMs = Number(process.argv[7] ?? 80);
const maxRecoveryMs = process.env.FERNSDR_LINK_MAX_RECOVERY_MS === undefined ? undefined :
  Number(process.env.FERNSDR_LINK_MAX_RECOVERY_MS);
assert([kbps, seconds, outageAt, outageMs, jitterMs].every(Number.isFinite));
assert(kbps > 0 && seconds >= 20 && Math.min(outageAt, outageMs, jitterMs) >= 0);
assert(maxRecoveryMs === undefined || (Number.isFinite(maxRecoveryMs) && maxRecoveryMs >= 0 && outageMs > 0));
const proxy = spawn('python3', [new URL('../../tools/link-proxy.py', import.meta.url).pathname,
  '--upstream', url.port || '80', '--kbps', String(kbps), '--jitter-ms', String(jitterMs),
  '--outage-at', String(outageAt), '--outage-ms', String(outageMs)], { stdio: ['ignore', 'pipe', 'inherit'] });
let browser;
try {
  await new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error('relay did not start within 15 seconds')), 15000);
    proxy.stdout.once('data', () => { clearTimeout(timeout); resolve(); });
    proxy.once('error', error => { clearTimeout(timeout); reject(error); });
    proxy.once('exit', code => { clearTimeout(timeout); reject(new Error(`relay exited: ${code}`)); });
  });
  browser = await chromium.launch();
  // The socket goes to the shaping relay on another port, which the page's
  // Content-Security-Policy rightly refuses; the policy is not what this
  // checks.
  const page = await browser.newPage({ bypassCSP: true });
  await page.addInitScript(() => {
    window.__stats = [];
    window.__arrivals = [];
    window.__segments = 0;
    const Socket = window.WebSocket;
    window.WebSocket = class extends Socket {
      constructor(url, ...args) {
        const relay = new URL(url); relay.port = '18074';
        super(relay, ...args);
        let rate = 0, generation = -1, previous = -1, firstAt = 0, samples = 0, segment = 0;
        this.addEventListener('message', ({ data }) => {
          if (typeof data === 'string') {
            const message = JSON.parse(data);
            if (message.type === 'audio-config' && (rate !== message.rate || generation !== message.generation)) {
              rate = message.rate;
              generation = message.generation;
              previous = -1;
            }
            return;
          }
          if (!(data instanceof ArrayBuffer) || data.byteLength < 4) return;
          const view = new DataView(data);
          if (view.getUint8(0) !== 1 || rate <= 0 || view.getUint8(1) >> 4 !== generation) return;
          const at = performance.now();
          const sequence = view.getUint16(2, true);
          if (previous < 0) {
            firstAt = at;
            samples = 0;
            segment = ++window.__segments;
          } else {
            const advance = (sequence - previous) & 0xffff;
            if (advance === 0 || advance > 0x8000) return;
            samples += advance * 128;
          }
          previous = sequence;
          // This is growth relative to the segment's first arrival, not
          // absolute antenna-to-speaker latency. A steady playback buffer
          // can conceal seconds of stale audio still travelling over TCP.
          window.__arrivals.push({ at, segment, rate, generation, sequence,
            discontinuity: (view.getUint8(1) & 4) !== 0,
            lagMs: at - firstAt - samples * 1000 / rate });
        });
      }
    };
    const Node = window.AudioWorkletNode;
    window.AudioWorkletNode = class extends Node {
      constructor(...args) {
        super(...args);
        this.port.addEventListener('message', (event) => {
          if (event.data.type === 'stats') window.__stats.push({ at: performance.now(), ...event.data });
        });
      }
    };
  });
  await page.goto(url.href);
  await page.locator('.audio-gate__card').click();
  await page.waitForTimeout(seconds * 1000);
  const trace = await page.evaluate(() => window.__stats);
  const arrivals = await page.evaluate(() => window.__arrivals);
  if (process.env.FERNSDR_LINK_TRACE) {
    await writeFile(process.env.FERNSDR_LINK_TRACE, JSON.stringify({ playback: trace, arrivals }, null, 2));
  }
  const settled = trace.filter(s => s.at > (seconds - 15) * 1000).map(s => s.latencyMs).sort((a, b) => a - b);
  const report = await page.evaluate(() => ({
    state: document.querySelector('.status__state')?.textContent,
    samples: window.__stats.length,
    first: window.__stats[0], last: window.__stats.at(-1),
    maxLatencyMs: Math.max(...window.__stats.map(s => s.latencyMs)),
  }));
  const quantile = (values, fraction) => values.sort((a, b) => a - b)[Math.min(values.length - 1, Math.floor(values.length * fraction))];
  const transport = [...new Set(arrivals.map(a => a.segment))].map(segment => {
    const frames = arrivals.filter(a => a.segment === segment);
    const start = frames[0].at, end = frames.at(-1).at;
    const baseline = quantile(frames.filter(a => a.at - start >= 2000 && a.at - start < 7000).map(a => a.lagMs), 0.5);
    const settled = frames.filter(a => a.at >= end - 15000).map(a => a.lagMs - baseline);
    let longestGapMs = 0, resumed = 0;
    for (let i = 1; i < frames.length; i++) {
      const gap = frames[i].at - frames[i - 1].at;
      if (gap > longestGapMs) { longestGapMs = gap; resumed = i; }
    }
    let recoveryTo500Ms;
    if (outageMs > 0 && longestGapMs >= outageMs * 0.75 && Number.isFinite(baseline)) {
      let lastLate = resumed - 1;
      for (let i = resumed; i < frames.length; i++) if (frames[i].lagMs - baseline > 500) lastLate = i;
      recoveryTo500Ms = lastLate + 1 < frames.length ? frames[lastLate + 1].at - frames[resumed].at : null;
    }
    return { segment, frames: frames.length, rate: frames[0].rate, durationMs: end - start,
      discontinuities: frames.filter(a => a.discontinuity).length,
      longestGapMs, recoveryTo500Ms,
      settledLagGrowthMs: quantile(settled, 0.5), settledP95LagGrowthMs: quantile(settled, 0.95),
      maxLagGrowthMs: frames.reduce((max, a) => Math.max(max, a.lagMs - baseline), -Infinity) };
  });
  console.log(JSON.stringify({ kbps, seconds, jitterMs, outageAt, outageMs, ...report, transport,
    settledMedianMs: settled[Math.floor(settled.length / 2)],
    settledP95Ms: settled[Math.floor(settled.length * 0.95)],
  }, null, 2));
  assert(report.samples > seconds * 2, 'playback stopped reporting');
  assert(report.last.decodedFrames > seconds * 50, 'insufficient audio received');
  assert(report.last.latencyMs < 400, 'decoded playback buffer remained above 400 ms');
  const measured = transport.filter(segment => segment.durationMs >= 15000);
  assert(measured.length > 0 && measured.every(segment => Number.isFinite(segment.settledP95LagGrowthMs)),
    'no stable audio clock segment with a finite transport baseline');
  assert(measured.every(segment => segment.settledP95LagGrowthMs < 500), 'transport remained over 500 ms behind its baseline');
  if (maxRecoveryMs !== undefined) {
    const recovered = measured.filter(segment => segment.recoveryTo500Ms !== undefined);
    assert(recovered.length > 0 && recovered.every(segment =>
      Number.isFinite(segment.recoveryTo500Ms) && segment.recoveryTo500Ms <= maxRecoveryMs),
    `transport did not recover within ${maxRecoveryMs} ms of delivery resuming`);
  }
  assert(!report.state?.includes('Disconnected'));
} finally {
  proxy.kill('SIGTERM');
  await browser?.close();
}
