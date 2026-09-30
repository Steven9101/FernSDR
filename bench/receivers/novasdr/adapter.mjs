// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Drives NovaSDR 0.3.7's own page (the React UI of its frontend submodule).
//
// The page opens /audio, /waterfall, /events (sometimes twice) and /chat.
// Tuning goes out on /audio as JSON text: {"cmd":"window","l","r","m"} in
// FFT bins, where m is the carrier (the dial), and
// {"cmd":"demodulation","demodulation":"USB"}. CW is sent as USB with a
// 400 Hz passband centred 750 Hz above m; FMC and WBFM are sent as FM.
// The first text frame on /audio gives basefreq, total_bandwidth and
// fft_result_size, which turn m back into hertz.
//
// Two things the page does on its own: when the dial enters a band of its
// band plan it switches the mode to the band's (LSB on 40 m), and it shows
// a "Start audio" overlay until the first pointer or key press.

export const id = 'novasdr';

const MODES = {usb: 'USB', lsb: 'LSB', am: 'AM', sam: 'SAM', fm: 'FM', cw: 'CW'};
// What the page puts on the wire for a mode it shows.
const WIRE = {USB: 'USB', LSB: 'LSB', AM: 'AM', SAM: 'SAM', FM: 'FM', FMC: 'FM', WBFM: 'FM', CW: 'USB'};

const state = new WeakMap();

function pathOf(url) {
  try {
    return new URL(url).pathname;
  } catch {
    return '';
  }
}

// Every text frame the page sends on /audio, and the settings it receives.
function track(page) {
  const s = {window: null, demodulation: null, info: null};
  page.on('websocket', (ws) => {
    if (pathOf(ws.url()) !== '/audio') return;
    ws.on('framesent', (f) => {
      if (typeof f.payload !== 'string') return;
      try {
        const msg = JSON.parse(f.payload);
        if (msg.cmd === 'window' && msg.m !== undefined) s.window = msg;
        if (msg.cmd === 'demodulation') s.demodulation = msg.demodulation;
      } catch {
        // not JSON: not a command
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

function modeTrigger(page) {
  // The Demodulation card's "Mode" dropdown: a button showing the mode.
  return page
    .locator('div', {has: page.locator(':scope > label', {hasText: /^Mode$/})})
    .locator(':scope > button')
    .first();
}

export async function open(page, baseUrl) {
  const s = track(page);
  await page.goto(baseUrl, {waitUntil: 'load'});
  await page.locator('#freq-khz').waitFor({state: 'visible', timeout: 20000});
  // Ready once the audio socket has its settings and the page has sent a
  // first window.
  for (let i = 0; i < 100 && !(s.info && s.window); i++) await page.waitForTimeout(100);
}

export async function startAudio(page) {
  // The page's own gate: a full-page overlay that listens for pointerdown.
  await page.getByRole('button', {name: /Start audio/}).click({timeout: 10000});
  await page.waitForTimeout(500);
}

export async function tune(page, {freq, mode}) {
  const want = MODES[mode];
  if (!want) throw new Error(`novasdr: no mode ${mode}`);
  const s = state.get(page);
  // Frequency first: entering a new band makes the page pick the band's mode.
  const input = page.locator('#freq-khz');
  await input.click();
  await input.fill((freq / 1000).toFixed(3));
  await input.press('Enter');
  await page.waitForTimeout(300);
  const trigger = modeTrigger(page);
  if ((await trigger.innerText()).trim() !== want) {
    await trigger.click();
    await page.getByRole('menuitem', {name: want, exact: true}).click();
  }
  // Wait until the page has put both on the wire.
  const hz = Math.round(freq);
  for (let i = 0; i < 50; i++) {
    const rb = s ? wireReadback(s) : null;
    if (rb && Math.abs(rb.freq - hz) <= 1 && s.demodulation === WIRE[want]) break;
    await page.waitForTimeout(100);
  }
  await page.waitForTimeout(200);
}

function wireReadback(s) {
  if (!s.info || !s.window) return null;
  const {basefreq, total_bandwidth: bw, fft_result_size: fft} = s.info;
  return {freq: Math.round(basefreq + (s.window.m * bw) / fft)};
}

export async function readback(page) {
  const s = state.get(page);
  const wire = s ? wireReadback(s) : null;
  const shown = (await modeTrigger(page).innerText().catch(() => '')).trim();
  const shownKhz = await page.locator('#freq-khz').inputValue().catch(() => null);
  // The mode as the page shows it, when what it sent agrees with it (CW
  // travels as USB); otherwise what it sent.
  const wireMode = s?.demodulation ?? null;
  const mode = shown && WIRE[shown] === wireMode ? shown.toLowerCase() : wireMode ? wireMode.toLowerCase() : null;
  return {
    freq: wire ? wire.freq : null,
    mode,
    sent: {window: s?.window ?? null, demodulation: wireMode},
    shown: {khz: shownKhz, mode: shown},
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
