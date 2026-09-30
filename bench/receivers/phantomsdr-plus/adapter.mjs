// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Drives the page PhantomSDR-Plus 2.0.0 ships in its release tarball.
//
// The page opens /chat, /waterfall, /audio and /events. Tuning goes out on
// /audio as JSON text in FFT bins, {"cmd":"window","l","m","r"} with m the
// dial, and {"cmd":"demodulation","demodulation":"USB"}; CW is sent as USB
// with a window offset from the dial. The first text frame on /audio gives
// basefreq, total_bandwidth and fft_result_size, which turn bins into hertz.
//
// On load the page shows a "Start Tutorial / Skip" dialog, and an "Enable
// Audio" overlay when the browser has not let its AudioContext run.

export const id = 'phantomsdr-plus';

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
  const freq = page.locator('input[name=frequency]');
  await freq.waitFor({state: 'visible', timeout: 20000});
  // Controls stay disabled until the backend connection is up.
  await page.waitForFunction(() => {
    const el = document.querySelector('input[name=frequency]');
    return el && !el.disabled;
  }, null, {timeout: 20000});
  // The tutorial dialog appears a moment after load, above everything else.
  const skip = page.getByRole('button', {name: 'Skip', exact: true});
  try {
    await skip.waitFor({state: 'visible', timeout: 8000});
    await skip.click();
    await skip.waitFor({state: 'hidden', timeout: 5000});
  } catch {
    // no tutorial this time (it remembers a skip in localStorage)
  }
  for (let i = 0; i < 100 && !(s.info && s.window); i++) await page.waitForTimeout(100);
}

export async function startAudio(page) {
  // The page's own gate, when it shows it: "Enable Audio / Tap anywhere to
  // start" (#startaudio), which listens for mousedown on the document.
  const gate = page.locator('#startaudio');
  if (await gate.isVisible().catch(() => false)) await gate.click();
  await page.waitForTimeout(500);
}

export async function tune(page, {freq, mode}) {
  const want = MODES[mode];
  if (!want) throw new Error(`phantomsdr-plus: no mode ${mode}`);
  const s = state.get(page);
  if (want === 'AM' || want === 'SAM') {
    // One button for both: a click selects whichever of AM and SAM it last
    // had (kept in localStorage), holding it for 500 ms opens a choice.
    const am = page.locator('.am-button').first();
    await am.hover();
    await page.mouse.down();
    await page.waitForTimeout(700);
    await page.mouse.up();
    await page.locator('.am-button-container').getByText(want, {exact: true}).last().click();
  } else {
    const button = page.getByRole('button', {name: want, exact: true});
    if (!(await button.isVisible().catch(() => false))) throw new Error(`phantomsdr-plus: no ${want} button`);
    await button.click();
  }
  await page.waitForTimeout(200);
  const input = page.locator('input[name=frequency]');
  await input.click();
  await input.fill((freq / 1000).toFixed(2));
  await input.press('Enter');
  const hz = Math.round(freq);
  for (let i = 0; i < 50; i++) {
    const rb = s ? wireFreq(s) : null;
    if (rb !== null && Math.abs(rb - hz) <= 1 && s.demodulation === WIRE[want]) break;
    await page.waitForTimeout(100);
  }
  await page.waitForTimeout(200);
}

function wireFreq(s) {
  if (!s.info || !s.window) return null;
  const {basefreq, total_bandwidth: bw, fft_result_size: fft} = s.info;
  return Math.round(basefreq + (s.window.m * bw) / fft);
}

async function shownMode(page) {
  return (await page.locator('.mode-text').first().innerText().catch(() => '')).trim();
}

export async function readback(page) {
  const s = state.get(page);
  const shown = await shownMode(page);
  const wireMode = s?.demodulation ?? null;
  const mode = shown && WIRE[shown] === wireMode ? shown.toLowerCase() : wireMode ? wireMode.toLowerCase() : null;
  return {
    freq: s ? wireFreq(s) : null,
    mode,
    sent: {window: s?.window ?? null, demodulation: wireMode},
    shown: {khz: await page.locator('input[name=frequency]').inputValue().catch(() => null), mode: shown},
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
