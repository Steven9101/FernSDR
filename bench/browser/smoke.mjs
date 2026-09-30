#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Does a receiver take the lab's input, and does its adapter drive it?
//
//   node bench/browser/smoke.mjs ADAPTER URL [--seconds 12] [--freq 7159000]
//        [--mode usb] [--tone 1000] [--out DIR]
//
// With the smoke scene playing, it opens the page, starts audio, tunes,
// listens through the tap, and reports: whether the marker is heard (its
// chip pattern found in the tone's envelope in most periods), the tone's
// pitch, how much audio the page played and whether the tap lost any,
// the traffic per stream, and what the adapter reads back. It prints JSON
// and exits non-zero when a check fails.
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {launch, countSockets, TapCollector, clockRatio, dominantTone, findMarker, writeWav} from './lib.mjs';

function parseArgs(argv) {
  const out = {seconds: 12, freq: 7159000, mode: 'usb', tone: 1000, out: null};
  const pos = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a.startsWith('--')) out[a.slice(2)] = argv[++i];
    else pos.push(a);
  }
  if (pos.length !== 2) {
    console.error('usage: smoke.mjs ADAPTER URL [--seconds 12] [--freq 7159000] [--mode usb] [--tone 1000] [--out DIR]');
    process.exit(2);
  }
  return {...out, adapter: pos[0], url: pos[1], seconds: Number(out.seconds), freq: Number(out.freq), tone: Number(out.tone)};
}

const args = parseArgs(process.argv.slice(2));
const adapter = await import(pathToFileURL(path.resolve(args.adapter)).href);
const {browser, context, tap} = await launch();
const page = await context.newPage();
const consoleErrors = [];
page.on('console', (m) => {
  if (m.type() === 'error') consoleErrors.push(m.text().slice(0, 300));
});
page.on('pageerror', (error) => consoleErrors.push(String(error).slice(0, 500)));
page.on('pageerror', (e) => consoleErrors.push(String(e).slice(0, 300)));
const sockets = countSockets(page, adapter.classify);

const report = {adapter: adapter.id, url: args.url, checks: {}};
const fail = (name, why) => {
  report.checks[name] = {ok: false, why};
};
const pass = (name, detail) => {
  report.checks[name] = {ok: true, ...detail};
};

try {
  await adapter.open(page, args.url);
  pass('open', {});
  await adapter.startAudio(page);
  await adapter.tune(page, {freq: args.freq, mode: args.mode});
  const started = Date.now();
  await page.waitForTimeout(args.seconds * 1000);
  const listened = (Date.now() - started) / 1000;
  const visible = await page.evaluate(() => document.visibilityState === 'visible');
  if (visible) pass('visibility', {});
  else fail('visibility', 'page is hidden');

  const rb = await adapter.readback(page);
  report.readback = rb;
  if (rb && Math.abs(rb.freq - args.freq) <= 10 && String(rb.mode).toLowerCase() === args.mode) pass('readback', rb);
  else fail('readback', `wanted ${args.freq} ${args.mode}, page says ${JSON.stringify(rb)}`);

  await tap.flush(page);
  const ctx = tap.loudest();
  const rec = TapCollector.assemble(ctx);
  report.audio = {
    contexts: [...tap.contexts.values()].map((c) => ({...c.info, lastClock: c.clocks.at(-1) ?? null})),
    media: tap.media,
    tapErrors: tap.errors,
    rate: rec.rate,
    seconds: rec.rate ? rec.x.length / rec.rate : 0,
    holesSeconds: rec.rate ? rec.holes / rec.rate : 0,
  };
  if (rec.rate && rec.x.length / rec.rate >= 0.8 * listened) pass('audio', {seconds: report.audio.seconds});
  else fail('audio', `played ${report.audio.seconds.toFixed(1)} s of ${listened.toFixed(1)} s`);

  if (rec.rate) {
    // Leave out the first two seconds: players fill their buffers.
    const skip = Math.min(rec.x.length, Math.round(2 * rec.rate));
    const x = rec.x.subarray(skip);
    const toneHz = dominantTone(x, rec.rate);
    report.toneHz = toneHz;
    if (toneHz !== null && Math.abs(toneHz - args.tone) <= 20) pass('pitch', {toneHz: Number(toneHz.toFixed(2))});
    else fail('pitch', `tone at ${toneHz} Hz, wanted ${args.tone} Hz`);
    const m = findMarker(x, rec.rate, {tone: toneHz ?? args.tone});
    const meanCorr = m.corrs.length ? m.corrs.reduce((a, b) => a + b, 0) / m.corrs.length : 0;
    report.marker = {found: m.found, periods: m.periods, meanCorr: Number(meanCorr.toFixed(3)), driftMsPerPeriod: m.driftMsPerPeriod === null ? null : Number(m.driftMsPerPeriod.toFixed(3))};
    if (m.periods && m.found / m.periods >= 0.8) pass('marker', report.marker);
    else fail('marker', `pattern in ${m.found} of ${m.periods} periods, mean correlation ${meanCorr.toFixed(3)}`);
    // Not a check of the receiver: whether the lab's audio clock held.
    const ratio = clockRatio(ctx);
    report.audioClockRatio = ratio;
    report.audioClockOk = ratio !== null && Math.abs(ratio - 1) <= 0.001;
    if (report.audioClockOk) pass('audio-clock', {ratio});
    else fail('audio-clock', `invalid laboratory playback clock: ${ratio}`);
    if (args.out) {
      fs.mkdirSync(args.out, {recursive: true});
      writeWav(path.join(args.out, `${adapter.id}-tap.wav`), rec.x, rec.rate);
    }
  }

  report.sockets = sockets.map((s) => ({
    url: s.url,
    closed: s.closed !== null,
    streams: Object.fromEntries(Object.entries(s.streams).map(([k, v]) => [k, {frames: v.frames, bytes: v.bytes, kbitPerS: Number(((v.bytes * 8) / 1000 / listened).toFixed(1))}])),
  }));
  const wf = sockets.flatMap((s) => Object.entries(s.streams)).filter(([k]) => k === 'waterfall:rx');
  const wfFrames = wf.reduce((a, [, v]) => a + v.frames, 0);
  if (wfFrames / listened >= 1) pass('waterfall', {framesPerS: Number((wfFrames / listened).toFixed(1))});
  else fail('waterfall', `${wfFrames} waterfall frames in ${listened.toFixed(1)} s`);
  const au = sockets.flatMap((s) => Object.entries(s.streams)).filter(([k]) => k === 'audio:rx');
  if (au.reduce((a, [, v]) => a + v.frames, 0) > 0) pass('audio-socket', {});
  else fail('audio-socket', 'no frame classified as audio');
  if (tap.errors.length) fail('tap', JSON.stringify(tap.errors));
  else pass('tap', {});
} catch (e) {
  fail('run', String(e && e.stack ? e.stack : e).slice(0, 800));
  if (args.out) {
    fs.mkdirSync(args.out, {recursive: true});
    fs.writeFileSync(path.join(args.out, 'page.html'), (await page.content()).slice(0, 262144));
    await page.screenshot({path: path.join(args.out, 'page.png')});
  }
} finally {
  report.consoleErrors = consoleErrors.slice(0, 20);
  report.ok = Object.values(report.checks).every((c) => c.ok);
  console.log(JSON.stringify(report, null, 1));
  await browser.close();
  process.exit(report.ok ? 0 : 1);
}
