#!/usr/bin/env node
// Calibration of tap frame placement, not physical sound-device latency.
import assert from 'node:assert/strict';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {launch, TapCollector, clockRatio, labPulse} from './lib.mjs';

const host = process.argv[2] ?? '127.0.0.1';
const out = process.argv[3];
const rate = Number(process.argv[4] ?? 48000);
if (![12000, 32000, 48000].includes(rate)) throw new Error('unsupported calibration rate');
if (out) fs.mkdirSync(out, {recursive: true});
if (!['127.0.0.1', '198.18.1.1'].includes(host)) throw new Error('use a loopback or lab client address');
const server = http.createServer((_, response) => {
  response.writeHead(200, {'Content-Type': 'text/html'});
  response.end('<!doctype html><title>Audio tap calibration</title><button>Start</button>');
});
await new Promise(resolve => server.listen(0, host, resolve));
const results = [];
let browser;
let monitor, monitorExit;
try {
  if (out) {
    monitor = spawn(fileURLToPath(new URL('../calibration/build/monitor', import.meta.url)),
                    [labPulse(), '30', path.join(out, 'monitor.f32'), path.join(out, 'monitor.jsonl')],
                    {stdio: ['ignore', 'ignore', 'inherit']});
    monitorExit = new Promise((resolve, reject) => { monitor.once('exit', resolve); monitor.once('error', reject); });
  }
  const launched = await launch();
  browser = launched.browser;
  const page = await launched.context.newPage();
  await page.goto(`http://${host}:${server.address().port}/`);
  await page.getByRole('button').click();
  const expected = await page.evaluate(async (rate) => {
    const ctx = new AudioContext({sampleRate: rate, latencyHint: 'interactive'});
    await ctx.resume();
    const base = Math.ceil(ctx.currentTime * ctx.sampleRate) + 3 * rate;
    const starts = [];
    for (let i = 0; i < 24; i++) {
      const source = ctx.createBufferSource();
      source.buffer = ctx.createBuffer(1, rate / 20, ctx.sampleRate);
      source.buffer.getChannelData(0).fill(0.25);
      const delay = ctx.createDelay(1);
      delay.delayTime.value = i % 2 ? 0.03 : 0;
      source.connect(delay);
      delay.connect(ctx.destination);
      const frame = base + i * rate / 2;
      source.start(frame / ctx.sampleRate);
      starts.push(frame + (i % 2 ? .03 * rate : 0));
    }
    return {starts, rate: ctx.sampleRate, secure: window.isSecureContext};
  }, rate);
  await page.waitForTimeout(17000);
  await launched.tap.flush(page);
  const ctx = launched.tap.loudest();
  const recording = TapCollector.assemble(ctx);
  const starts = [];
  let previous = false;
  for (let i = 0; i < recording.x.length; i++) {
    const present = recording.x[i] > 0.1;
    if (present && !previous) starts.push(recording.first + i);
    previous = present;
  }
  const offsets = starts.map((frame, i) => frame - expected.starts[i]);
  if (out) {
    fs.writeFileSync(path.join(out, 'tap.json'), JSON.stringify({expected, starts, clocks: ctx.clocks, info: ctx.info,
      firstFrame: recording.first, holes: recording.holes, errors: launched.tap.errors}));
    fs.writeFileSync(path.join(out, 'tap.f32'), Buffer.from(recording.x.buffer));
    monitor.kill('SIGTERM');
    assert.equal(await monitorExit, 0, 'independent monitor must finish without holes');
  }
  assert.equal(launched.tap.errors.length, 0, JSON.stringify(launched.tap.errors));
  assert.equal(starts.length, expected.starts.length, 'all scheduled pulses must be captured');
  assert.ok(offsets.every(offset => Math.abs(offset) <= 1), `frame errors: ${offsets}`);
  assert.ok(Math.abs(clockRatio(ctx)-1) <= .001, 'audio clock must run at the declared rate');
  results.push({host, tapKind: ctx.info.tapKind, secure: expected.secure,
                offsetsFrames: offsets, rate: recording.rate, clockRatio: clockRatio(ctx), holes: recording.holes});
  console.log(JSON.stringify({ok: true, gate: 'tap-frame-placement', physicalOutputCalibrated: false, results}, null, 2));
} finally {
  if (monitor && monitor.exitCode === null) { monitor.kill('SIGTERM'); await monitorExit; }
  await browser?.close();
  await new Promise(resolve => server.close(resolve));
}
