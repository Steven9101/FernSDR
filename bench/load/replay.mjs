#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Many listeners without browsers: each opens the sockets a real page
// opened (from a listen.mjs recording) and sends what the page sent, at the
// same moments, then reads everything the receiver sends back.
//
//   node bench/load/replay.mjs RECORDING ADAPTER URL --clients N --seconds S
//        [--window-from S] [--sources 198.18.100.2+200] --out FILE
//
// --sources gives each listener a source address of its own, round robin
// over COUNT addresses from the first (the client namespace has them): a
// crowd comes from many addresses, and receivers limit what one address
// may open (OpenWebRX+ serves no fifth socket an address opens within
// about ten seconds). Every socket also sends the Origin a page would.
// Sockets are the ws package's: undici's (Node's own) sends its handshake
// headers in lower case, which PA3FWM's WebSDR does not take, and browsers
// do not do.
//
// Every listener gets its own dial: the recorded one is replaced in text
// frames by a frequency drawn from the band (as Hz, kHz, MHz, or an offset
// from the 7.1 MHz centre), so that receivers that share work between
// listeners on one frequency do not get an easier load than a real crowd.
// Frames the substitution cannot read (binary tuning, or tuning in FFT
// bins) go out unchanged, and the output counts how many listeners were
// spread: "spread" is false when none of a listener's frames changed.
//
// For each listener it counts payload bytes per stream (the adapter's
// classify) inside the measurement window, and whether its sockets closed.
import fs from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {Agent, fetch} from 'undici';
import WebSocket from 'ws';

const argv = process.argv.slice(2);
const opt = {clients: 10, seconds: 20, 'window-from': 10, 'ramp-per-s': 20, dial: 7159200, seed: 1};
const pos = [];
for (let i = 0; i < argv.length; i++) {
  if (argv[i].startsWith('--')) opt[argv[i].slice(2)] = argv[++i];
  else pos.push(argv[i]);
}
const [recording, adapterPath, target] = pos;
const adapter = await import(pathToFileURL(path.resolve(adapterPath)).href);
// Only the page's sockets to the receiver: sv1btl's page also probes CAT
// bridges on localhost every few seconds, which a listener's browser
// answers itself and the receiver never sees.
const pageHost = new URL(JSON.parse(fs.readFileSync(path.join(recording, 'meta.json'), 'utf8')).url).hostname;
const sockets = JSON.parse(fs.readFileSync(path.join(recording, 'sockets.json'), 'utf8'))
  .filter((s) => (s.sent || s.streams) && new URL(s.url).hostname === pageHost)
  .sort((a, b) => a.opened - b.opened);
const origin = new URL(target);
const t0Page = Math.min(...sockets.map((s) => s.opened));
const clients = Number(opt.clients);
const seconds = Number(opt.seconds);
const windowFrom = Number(opt['window-from']);
const dial = Number(opt.dial);

let seed = Number(opt.seed);
const rand = () => ((seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648);

function substitute(text, from, to) {
  const hz = (x) => String(Math.round(x));
  const forms = [
    [hz(from), hz(to)],
    [(from / 1000).toFixed(1), (to / 1000).toFixed(1)],
    [(from / 1000).toFixed(3), (to / 1000).toFixed(3)],
    [(from / 1e6).toFixed(4), (to / 1e6).toFixed(4)],
    [(from / 1e6).toFixed(6), (to / 1e6).toFixed(6)],
    [hz(from - 7100000), hz(to - 7100000)],
  ];
  let out = text;
  let changed = false;
  for (const [a, b] of forms) {
    const re = new RegExp(`(?<![0-9.])${a.replace('.', '\\.')}(?![0-9])`, 'g');
    if (re.test(out)) {
      out = out.replace(re, b);
      changed = true;
    }
  }
  return {out, changed};
}

const results = [];
const start = Date.now();
const windowStart = start + windowFrom * 1000;
const end = start + seconds * 1000;

const [sourceFirst, sourceCount] = opt.sources ? opt.sources.split('+') : [null, 0];
function sourceAddress(i) {
  if (!sourceFirst) return null;
  const parts = sourceFirst.split('.').map(Number);
  parts[3] += i % Number(sourceCount);
  return parts.join('.');
}

function runClient(i) {
  const r = {client: i, source: sourceAddress(i), dial: null, spread: false, streams: {}, opened: 0, closedEarly: 0, errors: 0};
  const dispatcher = new Agent(r.source ? {localAddress: r.source} : {});
  // A dial from 7.02 to 7.18 MHz, on a 100 Hz grid.
  r.dial = Math.round((7.02e6 + rand() * 0.16e6) / 100) * 100;
  results.push(r);
  for (const s of sockets) {
    const url = new URL(s.url);
    url.host = origin.host;
    const delay = s.opened - t0Page;
    setTimeout(async () => {
      // UberSDR registers a session over HTTP first and names it in the
      // socket's URL: every listener needs its own.
      if (url.searchParams.has('user_session_id')) {
        const id = crypto.randomUUID();
        url.searchParams.set('user_session_id', id);
        try {
          await fetch(`${origin.protocol}//${origin.host}/connection`, {method: 'POST', dispatcher,
            headers: {'content-type': 'application/json', origin: origin.origin}, body: JSON.stringify({user_session_id: id})});
        } catch (e) {
          r.errors++;
          r.firstError ??= String(e?.cause?.message || e).slice(0, 200);
        }
      }
      let ws;
      try {
        // Host first, as browsers send it: VertexSDR looks for it only in the
        // lines before Sec-WebSocket-Key, and ws otherwise adds it last.
        ws = new WebSocket(url.toString(), {headers: {Host: url.host, Origin: origin.origin}, ...(r.source ? {localAddress: r.source} : {})});
      } catch (e) {
        r.errors++;
        r.firstError ??= String(e).slice(0, 200);
        return;
      }
      ws.binaryType = 'arraybuffer';
      const openedAt = Date.now();
      r.opened++;
      ws.onopen = () => {
        for (const f of s.sent || []) {
          setTimeout(() => {
            if (ws.readyState !== WebSocket.OPEN) return;
            if (f.text !== null) {
              const {out, changed} = substitute(f.text, dial, r.dial);
              if (changed) r.spread = true;
              ws.send(out);
            } else {
              ws.send(Buffer.from(f.b64, 'base64'));
            }
          }, Math.max(0, f.t - s.opened));
        }
      };
      ws.onmessage = (ev) => {
        const now = Date.now();
        if (now < windowStart || now > end) return;
        const frame = typeof ev.data === 'string' ? ev.data : new Uint8Array(ev.data);
        let cls = 'other';
        try {
          cls = adapter.classify(url.toString(), frame) || 'other';
        } catch (e) {
          cls = 'unclassified';
        }
        const n = typeof ev.data === 'string' ? Buffer.byteLength(ev.data) : ev.data.byteLength;
        const k = (r.streams[cls] ??= {frames: 0, bytes: 0});
        k.frames++;
        k.bytes += n;
      };
      ws.onerror = (ev) => {
        r.errors++;
        r.firstError ??= String(ev?.error?.message || ev?.message || 'socket error').slice(0, 200);
      };
      ws.onclose = (ev) => {
        if (Date.now() < end) {
          r.closedEarly++;
          r.firstClose ??= `${ev?.code} ${ev?.reason || ''}`.trim();
        }
      };
      setTimeout(() => ws.close(), Math.max(0, end - Date.now()));
    }, delay);
  }
}

const ramp = Number(opt['ramp-per-s']);
for (let i = 0; i < clients; i++) setTimeout(() => runClient(i), (i / ramp) * 1000);
setTimeout(() => {
  fs.writeFileSync(opt.out, JSON.stringify({clients, seconds, windowSeconds: (end - windowStart) / 1000,
    windowStartMs: windowStart, windowEndMs: end, results}));
  process.exit(0);
}, seconds * 1000 + 1500);
