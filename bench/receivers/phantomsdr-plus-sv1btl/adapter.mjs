// SPDX-License-Identifier: AGPL-3.0-or-later
// sv1btl 0fb2ee0: public mode controls and wire tuning readback.
export const id = 'phantomsdr-plus-sv1btl';

const MODES = {usb: 'USB', lsb: 'LSB', am: 'AM', fm: 'FM', cw: 'CW', sam: 'SAM'};
const WIRE = {USB: 'USB', LSB: 'LSB', AM: 'AM', SAM: 'SAM', FM: 'FM', CW: 'USB'};

const state = new WeakMap();

function pathOf(url) {
  try {
    return new URL(url).pathname;
  } catch {
    return '';
  }
}

function track(page) {
  const s = {window: null, demodulation: null, demodulationSentMs: [], info: null};
  page.on('websocket', (ws) => {
    if (pathOf(ws.url()) !== '/audio') return;
    ws.on('framesent', (f) => {
      if (typeof f.payload !== 'string') return;
      try {
        const msg = JSON.parse(f.payload);
        if (msg.cmd === 'window' && typeof msg.m === 'number') s.window = msg;
        if (msg.cmd === 'demodulation') {
          s.demodulation = msg.demodulation;
          s.demodulationSentMs.push(Date.now());
        }
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
  track(page);
  await page.goto(baseUrl, {waitUntil: 'domcontentloaded'});
  await page.waitForFunction(() => typeof window.setfreq === 'function' && typeof window.set_mode === 'function');
}

export async function startAudio(page) {
  const gate = page.locator('#startaudio');
  if (await gate.isVisible()) await gate.click();
}

export async function tune(page, {freq, mode}) {
  if (!MODES[mode]) throw new Error(`unsupported mode ${mode}`);
  // The shipped CATsync API calls the same tuning implementation as the UI.
  // Its setfreq accepts Hz above 1 MHz; pass kHz for an unambiguous unit.
  await page.evaluate(({freq}) => window.setfreq(freq / 1000), {freq});
  const s = state.get(page);
  for (let i = 0; i < 50 && Math.abs((wireFreq(s) ?? 0) - freq) > 1; i++) await page.waitForTimeout(100);
  // A retune through setfreq lets the band plan pick its mode (LSB on 40 m)
  // and then puts the old mode back a few milliseconds later; the server
  // drops a demodulation command that comes within 100 ms of the one before
  // (src/signal.cpp, on_demodulation_message), so it stays in the band
  // plan's mode while the page shows the other. Set the mode apart from
  // that, through a different mode so that set_mode does not see a repeat,
  // each command more than 100 ms after the last.
  const target = mode === 'cw' ? 'cwu' : mode;
  await page.waitForTimeout(250);
  await page.evaluate((m) => window.set_mode(m), target === 'am' ? 'fm' : 'am');
  await page.waitForTimeout(250);
  await page.evaluate((m) => window.set_mode(m), target);
  await page.waitForTimeout(250);
}

function wireFreq(s) {
  if (!s.info || !s.window) return null;
  const {basefreq, total_bandwidth: bw, fft_result_size: fft} = s.info;
  return Math.round(basefreq + (s.window.m * bw) / fft);
}

export async function readback(page) {
  const s = state.get(page);
  const shown = await page.evaluate(() => ({freq: window.ext_get_freq_kHz() * 1000, mode: window.ext_get_mode()}));
  const mode = {cwu: 'cw', cwl: 'cwl', nfm: 'fm'}[shown.mode] ?? shown.mode;
  // What the server accepted is not visible on the wire; the gap before the
  // last demodulation command says whether its debounce could have dropped it.
  const t = s.demodulationSentMs;
  const lastGapMs = t.length >= 2 ? t[t.length - 1] - t[t.length - 2] : null;
  return {freq: wireFreq(s), mode, sent: {window: s.window, demodulation: s.demodulation, lastGapMs}, shown};
}

export function classify(url, frame) {
  const path = pathOf(url);
  const text = typeof frame === 'string';
  if (path === '/audio') return text ? 'control' : 'audio';
  if (path === '/waterfall') return text ? 'control' : 'waterfall';
  if (path === '/events') return 'control';
  return 'other';
}
