// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Drives PhantomSDR's own page (html-svelte/dist at fb595bd).
//
// The page opens /waterfall, /audio and /events. Tuning goes out on /audio
// as JSON text in FFT bins, {"cmd":"window","l","m","r"}, and
// {"cmd":"demodulation","demodulation":"USB"}; the first text frame on
// /audio gives basefreq, total_bandwidth and fft_result_size.
//
// Its modes are radio buttons: USB, LSB, CW-U, CW-L, AM, FM. CW-U is sent as
// USB with m 700 Hz below the dial the page shows (its BFO) and a passband
// from m+500 to m+1000 Hz, so a carrier on the dial sounds at 700 Hz.
//
// Its frequency entry is a number input in MHz inside a hidden box, next to
// one number input per digit; the hidden input takes a value through the
// native setter and the input and change events Svelte listens for. Audio
// starts on the first mousedown anywhere, which also removes the START
// AUDIO overlay.

export const id = 'phantomsdr';

const MODES = {usb: 'USB', lsb: 'LSB', am: 'AM', fm: 'FM', cw: 'CW-U'};
// What the page puts on the wire for each of its modes, and the BFO it
// applies (m = dial + bfo), from its demodulationDefaults.
const WIRE = {USB: 'USB', LSB: 'LSB', 'CW-U': 'USB', 'CW-L': 'LSB', AM: 'AM', FM: 'FM', WBFM: 'FM'};
const BFO = {'CW-U': -700, 'CW-L': 700};
const NAMES = {USB: 'usb', LSB: 'lsb', 'CW-U': 'cw', 'CW-L': 'cw', AM: 'am', FM: 'fm', WBFM: 'fm'};

const state = new WeakMap();

function pathOf(url) {
  try {
    return new URL(url).pathname;
  } catch {
    return '';
  }
}

function track(page) {
  const s = {window: null, demodulation: null, info: null};
  page.on('websocket', (ws) => {
    if (pathOf(ws.url()) !== '/audio') return;
    ws.on('framesent', (f) => {
      if (typeof f.payload !== 'string') return;
      try {
        const msg = JSON.parse(f.payload);
        if (msg.cmd === 'window' && typeof msg.m === 'number') s.window = msg;
        if (msg.cmd === 'demodulation') s.demodulation = msg.demodulation;
      } catch {
        // not a command
      }
    });
    ws.on('framereceived', (f) => {
      if (typeof f.payload !== 'string') return;
      try {
        const msg = JSON.parse(f.payload);
        if (typeof msg.basefreq === 'number') s.info = msg;
      } catch {
        // ignore
      }
    });
  });
  state.set(page, s);
  return s;
}

export async function open(page, baseUrl) {
  const s = track(page);
  await page.goto(baseUrl, {waitUntil: 'load'});
  // Its inputs stay disabled until the backend connection is up.
  await page.waitForFunction(() => {
    const r = document.querySelector('input[name=demodulation]');
    return r && !r.disabled;
  }, null, {timeout: 20000});
  for (let i = 0; i < 100 && !(s.info && s.window); i++) await page.waitForTimeout(100);
}

export async function startAudio(page) {
  const button = page.getByRole('button', {name: 'START AUDIO'});
  if (await button.isVisible().catch(() => false)) await button.click();
  await page.waitForTimeout(500);
}

function mhz(hz) {
  return (hz / 1e6).toFixed(6);
}

export async function tune(page, {freq, mode}) {
  const want = MODES[mode];
  if (!want) throw new Error(`phantomsdr: no mode ${mode}`);
  const s = state.get(page);
  const radio = page.locator(`input[name=demodulation][value="${want}"]`);
  if (!(await radio.isChecked())) {
    await page.locator('label', {has: radio}).click();
    await page.waitForTimeout(200);
  }
  const ok = await page.evaluate((value) => {
    const box = [...document.querySelectorAll('input[type=number]')].find((el) => el.closest('[hidden]'));
    if (!box) return false;
    const setter = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
    setter.call(box, value);
    box.dispatchEvent(new Event('input', {bubbles: true}));
    box.dispatchEvent(new Event('change', {bubbles: true}));
    return true;
  }, mhz(freq));
  if (!ok) throw new Error('phantomsdr: no frequency box');
  const hz = Math.round(freq);
  for (let i = 0; i < 50; i++) {
    const rb = s ? dial(s, want) : null;
    if (rb !== null && Math.abs(rb - hz) <= 1 && s.demodulation === WIRE[want]) break;
    await page.waitForTimeout(100);
  }
  await page.waitForTimeout(200);
}

// The dial the page's last window stands for: m in hertz, less the mode's BFO.
function dial(s, pageMode) {
  if (!s.info || !s.window) return null;
  const {basefreq, total_bandwidth: bw, fft_result_size: fft} = s.info;
  return Math.round(basefreq + (s.window.m * bw) / fft - (BFO[pageMode] ?? 0));
}

export async function readback(page) {
  const s = state.get(page);
  const checked = await page
    .locator('input[name=demodulation]:checked')
    .getAttribute('value')
    .catch(() => null);
  const wireMode = s?.demodulation ?? null;
  // The page's mode when what it sent agrees with it (CW-U travels as USB).
  const mode = checked && WIRE[checked] === wireMode ? NAMES[checked] : wireMode ? wireMode.toLowerCase() : null;
  return {
    freq: s ? dial(s, checked) : null,
    mode,
    sent: {window: s?.window ?? null, demodulation: wireMode},
    shown: {mode: checked},
  };
}

export function classify(url, frame) {
  const path = pathOf(url);
  const text = typeof frame === 'string';
  if (path === '/audio') return text ? 'control' : 'audio';
  if (path === '/waterfall') return text ? 'control' : 'waterfall';
  if (path === '/events') return 'control';
  return 'other';
}
