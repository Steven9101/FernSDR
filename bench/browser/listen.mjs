#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Listens to a receiver as one listener and keeps the raw data for
// analysis:
//
//   node bench/browser/listen.mjs ADAPTER URL --out DIR [--seconds 60]
//        [--freq 7159000] [--mode usb] [--width 1920 --height 1080] [--barrier FILE]
//
// With --barrier, once warmed up it writes ready.json into DIR and opens its
// window only when FILE exists: harness/links.py shapes the link in between,
// so that no part of the window runs on an unshaped link.
// DIR gets audio.f32 (what the page played, from the audio context that
// played most, NaN where the tap got nothing), audio.json (its rate and
// first context frame), clocks.json (that context's getOutputTimestamp
// pairs, with performance.timeOrigin), sockets.json (every WebSocket frame's
// time, size and stream, as the adapter classifies it) and meta.json
// (tuning, read-back, wall-clock times, console and tap errors).
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {launch, countSockets, TapCollector, clockRatio, dominantTone} from './lib.mjs';

const argv = process.argv.slice(2);
const opt = {seconds: 60, warmup: 3, freq: 7159000, mode: 'usb', width: 1920, height: 1080};
const pos = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i].startsWith('--')) opt[argv[i].slice(2)] = argv[++i];
  else pos.push(argv[i]);
}
if (pos.length !== 2 || !opt.out) {
  console.error('usage: listen.mjs ADAPTER URL --out DIR [--seconds 60] [--freq F] [--mode M]');
  process.exit(2);
}
if (!Number.isFinite(Number(opt.seconds)) || Number(opt.seconds) < 10 || Number(opt.seconds) > 300)
  throw new Error('capture duration must be 10..300 seconds');
const adapter = await import(pathToFileURL(path.resolve(pos[0])).href);
fs.mkdirSync(opt.out, {recursive: true});
const {browser, context, tap} = await launch({width: Number(opt.width), height: Number(opt.height)});
const page = await context.newPage();
const consoleErrors = [];
page.on('console', (m) => {
  if (m.type() === 'error' && consoleErrors.length < 100) consoleErrors.push(m.text().slice(0, 300));
});
page.on('pageerror', (e) => { if (consoleErrors.length < 100) consoleErrors.push(String(e).slice(0, 300)); });
const sockets = countSockets(page, adapter.classify);
const meta = {adapter: adapter.id, url: pos[1], freq: Number(opt.freq), mode: opt.mode, seconds: Number(opt.seconds)};
const requests = [];
const pendingRequests = [];
let requestOverflow = false;
page.on('requestfinished', request => {
  if (pendingRequests.length >= 10000) { requestOverflow = true; return; }
  pendingRequests.push((async () => {
    const response = await request.response();
    requests.push({url: request.url(), resourceType: request.resourceType(), timing: request.timing(),
                   sizes: await request.sizes(), status: response?.status(),
                   cacheControl: (await response?.allHeaders())?.['cache-control'] ?? null});
  })().catch(error => requests.push({url: request.url(), error: String(error)})));
});
page.on('requestfailed', request => {
  if (requests.length >= 10000) { requestOverflow = true; return; }
  requests.push({url: request.url(), failure: request.failure()});
});
await context.addInitScript(() => {
  const visibility = () => window.__fbTapSend('visibility', {state: document.visibilityState, wallMs: Date.now()});
  document.addEventListener('visibilitychange', visibility);
  visibility();
  window.__fbFrames = [];
  let last;
  function frame(now) {
    if (last !== undefined && window.__fbFrames.length < 20000) window.__fbFrames.push([performance.timeOrigin + now, now-last]);
    last = now;
    requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
});
let status = 0;
const frames = () => page.evaluate(() => Object.fromEntries(
  (window.__fbTap?.contexts ?? []).map(e => [e.id, Math.round(e.ctx.currentTime * e.ctx.sampleRate)])));
try {
  meta.openedWallMs = Date.now();
  await adapter.open(page, pos[1]);
  await adapter.startAudio(page);
  await adapter.tune(page, {freq: meta.freq, mode: meta.mode});
  meta.tunedWallMs = Date.now();
  // Players fill their buffers and AGCs settle before the window opens.
  meta.warmup = Number(opt.warmup);
  await page.waitForTimeout(meta.warmup * 1000);
  if (opt.barrier) {
    fs.writeFileSync(path.join(opt.out, 'ready.json'), JSON.stringify({readyWallMs: Date.now()}));
    const until = Date.now() + 180000;
    while (!fs.existsSync(opt.barrier)) {
      if (Date.now() > until) throw new Error(`no ${opt.barrier} within 180 s`);
      await page.waitForTimeout(100);
    }
  }
  meta.startFrames = await frames();
  meta.measurementStartedWallMs = Date.now();
  // Timed link impairments start from here (harness/links.py waits for it).
  fs.writeFileSync(path.join(opt.out, 'window.json'), JSON.stringify({startedWallMs: meta.measurementStartedWallMs}));
  await page.waitForTimeout(meta.seconds * 1000);
  meta.endedWallMs = Date.now();
  meta.endFrames = await frames();
  // Let the recorder deliver its last partial measurement block.
  await page.waitForTimeout(500);
  meta.readback = await adapter.readback(page);
  meta.visibility = await page.evaluate(() => document.visibilityState);
  meta.frameIntervals = await page.evaluate(() => window.__fbFrames);
  meta.navigation = await page.evaluate(() => performance.getEntriesByType('navigation').map(e => e.toJSON()));
} catch (e) {
  meta.error = String(e && e.stack ? e.stack : e).slice(0, 1000);
  status = 1;
  // What the page showed when the adapter gave up, for the diagnosis.
  try {
    fs.writeFileSync(path.join(opt.out, 'failed-page.html'), (await page.content()).slice(0, 262144));
    await page.screenshot({path: path.join(opt.out, 'failed-page.png'), timeout: 10000});
  } catch (error) {
    meta.screenshotError = String(error).slice(0, 300);
  }
} finally {
  try { await tap.flush(page); } catch (error) { tap.errors.push(String(error)); }
  const ctx = tap.loudest();
  const windowFor = c => ({first: meta.startFrames?.[c.info.id] ?? 0,
                           end: meta.endFrames?.[c.info.id] ?? (meta.endedWallMs ? Infinity : 0)});
  const rec = TapCollector.assemble(ctx, ctx ? windowFor(ctx) : null);
  meta.clockRatio = clockRatio(ctx);
  const expectedTone = Number(opt.tone ?? 800);
  meta.toneHz = rec.rate && rec.x.length ? dominantTone(rec.x, rec.rate, expectedTone-60, expectedTone+60) : null;
  const measuredFrames = (meta.frameIntervals ?? []).filter(([t]) => t >= meta.measurementStartedWallMs && t <= meta.endedWallMs);
  meta.animationFps = measuredFrames.length / meta.seconds;
  meta.gates = {
    audio: rec.rate > 0 && rec.x.length / rec.rate >= meta.seconds * .8,
    // lastGapMs, where an adapter reports it: a mode command sent within a
    // receiver's debounce of the one before may have been dropped.
    tuning: meta.readback?.freq != null && Math.abs(meta.readback.freq - meta.freq) <= 10 && meta.readback.mode === meta.mode
      && (meta.readback.sent?.lastGapMs ?? Infinity) >= 100,
    visibility: meta.visibility === 'visible' && tap.visibility.every(e => e.state === 'visible'),
    tap: tap.errors.length === 0 && rec.holes === 0,
    clock: meta.clockRatio !== null && Math.abs(meta.clockRatio - 1) <= .001,
    pitch: meta.toneHz !== null && Math.abs(meta.toneHz - expectedTone) <= 20,
    animation: meta.animationFps >= 20,
    collection: !requestOverflow && !tap.overflow && !sockets.overflow,
  };
  if (!Object.values(meta.gates).every(Boolean)) status = 1;
  await Promise.all(pendingRequests);
  fs.writeFileSync(path.join(opt.out, 'requests.json'), JSON.stringify(requests));
  fs.writeFileSync(path.join(opt.out, 'audio.f32'), Buffer.from(rec.x.buffer, rec.x.byteOffset, rec.x.byteLength));
  fs.writeFileSync(path.join(opt.out, 'audio.json'), JSON.stringify({rate: rec.rate, firstFrame: rec.first, holes: rec.holes, contextId: ctx ? ctx.info.id : null}));
  fs.writeFileSync(path.join(opt.out, 'clocks.json'), JSON.stringify(ctx ? ctx.clocks : []));
  fs.writeFileSync(path.join(opt.out, 'contexts.json'), JSON.stringify([...tap.contexts.values()].map((c) => c.info)));
  meta.tracks = [];
  for (const [id, c] of tap.contexts) {
    const track = TapCollector.assemble(c, windowFor(c));
    if (!track.x.length) continue;
    const name = `track-${meta.tracks.length}`;
    fs.writeFileSync(path.join(opt.out, `${name}.f32`), Buffer.from(track.x.buffer, track.x.byteOffset, track.x.byteLength));
    fs.writeFileSync(path.join(opt.out, `${name}.json`), JSON.stringify({id, rate: track.rate, firstFrame: track.first,
      holes: track.holes, clocks: c.clocks, info: c.info}));
    meta.tracks.push(name);
  }
  fs.writeFileSync(path.join(opt.out, 'sockets.json'), JSON.stringify(sockets));
  meta.consoleErrors = consoleErrors.slice(0, 50);
  meta.tapErrors = tap.errors;
  meta.media = tap.media;
  meta.visibilityEvents = tap.visibility;
  fs.writeFileSync(path.join(opt.out, 'meta.json'), JSON.stringify(meta, null, 1));
  await browser.close();
  process.exit(status);
}
