// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Drives VertexSDR's page: PA3FWM's WebSDR page (pub2/, fetched and patched
// by scripts/fetch-frontend.sh) as a listener uses it. "start audio" is
// VertexSDR's own button; the Frequency box takes kHz and Enter; the mode
// buttons set the mode with the page's default filter for it.
//
// The page speaks for the carrier it tunes and shows a "nominal" frequency:
// with a passband narrower than 1 kHz (CW) the dial is the middle of the
// passband, so CW-wide (-0.95 to -0.55 kHz, lower side) tunes the receiver
// 750 Hz above the dial and a carrier exactly at the dial sounds at 750 Hz
// (websdr-base.js: iscw, nominalfreq, setfreqb).

export const id = 'vertexsdr';

// The lab's mode names to the page's buttons (pub2/index.html).
const MODE_BUTTONS = {usb: 'USB', lsb: 'LSB', cw: 'CW-wide', am: 'AM', sam: 'AMSYNC', fm: 'FM'};

// The last tuning message the page sent on its audio socket, per page.
const lastParam = new WeakMap();

export async function open(page, baseUrl) {
  page.on('websocket', (ws) => {
    if (!ws.url().includes('/~~stream')) return;
    ws.on('framesent', (f) => {
      // The same message also carries mute, squelch and the like alone.
      if (typeof f.payload === 'string' && f.payload.startsWith('GET /~~param?f=')) lastParam.set(page, f.payload);
    });
  });
  await page.goto(baseUrl, {waitUntil: 'load'});
  // The page fills the box once it knows its bands (tmp/bandinfo.js) and
  // has tuned to the configured initial frequency.
  await page.waitForFunction(() => document.freqform && document.freqform.frequency.value !== '', null, {timeout: 30000});
}

export async function startAudio(page) {
  // VertexSDR's green "start audio" button beside the HTML5/Java choice; the
  // same label on the hidden overlay is left alone.
  await page.locator('#html5choice input[type="button"][value="start audio"]').click();
}

export async function tune(page, {freq, mode}) {
  const button = MODE_BUTTONS[mode];
  if (!button) throw new Error(`vertexsdr: no mode ${mode}`);
  // Mode first, so that the frequency typed next is taken as the dial for
  // that mode's passband.
  await page.locator(`input[type="button"][value="${button}"]`).first().click();
  const box = page.locator('form[name="freqform"] input[name="frequency"]');
  await box.fill(String(freq / 1000));
  await box.press('Enter');
  // Done when the page has told the server; readback shows it if never.
  for (let waited = 0; waited < 5000; waited += 100) {
    const sent = lastParam.get(page);
    const now = sent && parseParam(sent);
    if (now && Math.abs(now.freq - freq) <= 1 && now.mode === mode) return;
    await page.waitForTimeout(100);
  }
}

function parseParam(text) {
  const q = new URLSearchParams(text.replace(/^GET \/~~param\?/, '').split(' ')[0]);
  if (!['f', 'lo', 'hi', 'mode'].every((k) => q.has(k))) return null;
  const f = Number(q.get('f'));
  const lo = Number(q.get('lo'));
  const hi = Number(q.get('hi'));
  const m = Number(q.get('mode'));
  if (![f, lo, hi, m].every(Number.isFinite)) return null;
  const cw = hi - lo < 1.0;
  let mode = {1: 'am', 2: 'sam', 4: 'fm'}[m];
  if (m === 0) mode = cw ? 'cw' : hi > 0 ? 'usb' : 'lsb';
  const dialKhz = cw ? f + (hi + lo) / 2 : f;
  return {freq: Math.round(dialKhz * 1000), mode: mode ?? `mode ${m}`};
}

export async function readback(page) {
  const sent = lastParam.get(page);
  if (sent) {
    const parsed = parseParam(sent);
    if (parsed) return parsed;
  }
  // Nothing sent yet: what the Frequency box shows, in kHz.
  const shown = await page.locator('form[name="freqform"] input[name="frequency"]').inputValue();
  return {freq: Math.round(parseFloat(shown) * 1000), mode: null};
}

// Separate sockets: /~~stream carries audio down (binary) and the page's
// "GET /~~param?..." tuning up (text); /~~waterstreamN carries the
// waterfall.
export function classify(url, frame) {
  if (url.includes('/~~waterstream')) return 'waterfall';
  if (url.includes('/~~stream')) return typeof frame === 'string' ? 'control' : 'audio';
  return 'other';
}
