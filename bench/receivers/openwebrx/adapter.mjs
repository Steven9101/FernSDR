// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Drives OpenWebRX 1.2.2's own page the way a listener does: the mode
// buttons and the frequency display in the receiver panel. The page keeps
// one WebSocket, /ws/, for everything; its JSON text messages are control,
// and a binary message's first byte says what it carries (htdocs/openwebrx.js,
// on_ws_recv): 1 waterfall, 2 audio, 3 the digital modes' secondary
// waterfall, 4 HD audio (WFM).
//
// Tuning never touches the profile list: a `selectprofile` message retunes
// the SDR for every listener on it.

export const id = 'openwebrx';

// Lab mode names to OpenWebRX modulations. 1.2.2 has no synchronous AM.
const MODULATION = {usb: 'usb', lsb: 'lsb', am: 'am', fm: 'nfm', cw: 'cw'};
const MODE = Object.fromEntries(Object.entries(MODULATION).map(([k, v]) => [v, k]));

const PANEL = '#openwebrx-panel-receiver';
const DIAL = `${PANEL} .webrx-actual-freq`;

// What the page last told the server (dspcontrol params, merged) and the
// centre frequency the server last announced, per page.
const traffic = new WeakMap();

function watch(page) {
  const t = {center: null, sent: {}};
  traffic.set(page, t);
  page.on('websocket', (ws) => {
    if (!/\/ws\/?$/.test(new URL(ws.url()).pathname)) return;
    // A new socket starts a new server-side session with nothing set yet.
    t.sent = {};
    ws.on('framesent', ({payload}) => {
      const m = parseJson(payload);
      if (m && m.type === 'dspcontrol' && m.params) Object.assign(t.sent, m.params);
    });
    ws.on('framereceived', ({payload}) => {
      const m = parseJson(payload);
      if (m && m.type === 'config' && m.value && 'center_freq' in m.value) t.center = m.value.center_freq;
    });
  });
}

function parseJson(payload) {
  if (typeof payload !== 'string' || payload[0] !== '{') return null;
  try {
    return JSON.parse(payload);
  } catch {
    return null;
  }
}

export async function open(page, baseUrl) {
  watch(page);
  await page.goto(baseUrl, {waitUntil: 'domcontentloaded'});
  // Ready once the receiver panel shows a dial and a selected mode, which
  // happens after the server's config message has started a demodulator.
  // No other overlay covers the page; the autoplay one is for startAudio.
  await page.waitForFunction(
    ([panel, dial]) => {
      const on = document.querySelector(`${panel} .openwebrx-demodulator-button.highlighted[data-modulation]`);
      const shown = document.querySelector(dial);
      return Boolean(on && shown && /[1-9]/.test(shown.textContent));
    },
    [PANEL, DIAL],
    {timeout: 30000},
  );
}

export async function startAudio(page) {
  // The page shows this overlay only when the browser blocks autoplay;
  // a click on it resumes the audio context.
  const overlay = page.locator('#openwebrx-autoplay-overlay');
  if (await overlay.isVisible()) await overlay.click();
  await page.waitForFunction(
    () => window.audioEngine && window.audioEngine.started && window.audioEngine.audioContext.state === 'running',
    null,
    {timeout: 15000},
  );
}

export async function tune(page, {freq, mode}) {
  const modulation = MODULATION[mode];
  if (!modulation) throw new Error(`OpenWebRX 1.2.2 has no ${mode} mode`);
  // Mode first: a new mode keeps the demodulator's offset, so the dial
  // typed afterwards is the one that stays.
  await page.locator(`${PANEL} .openwebrx-demodulator-button[data-modulation="${modulation}"]`).click();
  // The dial is the page's own entry field: a click opens it, the unit
  // list switches it to Hz, Enter submits.
  const dial = page.locator(DIAL);
  await dial.click();
  await dial.locator('select').selectOption('0');
  await dial.locator('input').fill(String(Math.round(freq)));
  await dial.locator('input').press('Enter');
  const t = traffic.get(page);
  await page.waitForFunction(
    ([dialSel, panel, want, wantMod]) => {
      const shown = $(dialSel).tuneableFrequencyDisplay().frequency;
      const on = document.querySelector(`${panel} .openwebrx-demodulator-button.highlighted[data-modulation]`);
      return shown === want && on && on.dataset.modulation === wantMod;
    },
    [DIAL, PANEL, Math.round(freq), modulation],
    {timeout: 10000},
  );
  // And the server has been told the same (the control message goes out
  // from the same call that updates the dial).
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (t.center !== null && t.center + t.sent.offset_freq === Math.round(freq) && t.sent.mod === modulation) return;
    await page.waitForTimeout(50);
  }
  throw new Error(`the page shows ${freq} ${mode} but told the server ${JSON.stringify({center: t.center, ...t.sent})}`);
}

export async function readback(page) {
  const shown = await page.evaluate(
    ([dialSel, panel]) => {
      const on = document.querySelector(`${panel} .openwebrx-demodulator-button.highlighted[data-modulation]`);
      return {
        freq: $(dialSel).tuneableFrequencyDisplay().frequency,
        // The display's own digits and unit, without its hidden entry field.
        text: document.querySelector(`${dialSel} > div:not(.input-group)`).textContent.trim(),
        modulation: on ? on.dataset.modulation : null,
      };
    },
    [DIAL, PANEL],
  );
  const t = traffic.get(page);
  return {
    freq: shown.freq,
    mode: MODE[shown.modulation] ?? shown.modulation,
    shown: shown.text,
    // What the server demodulates: its centre plus the offset last sent.
    control:
      t && t.center !== null && 'offset_freq' in t.sent
        ? {freq: t.center + t.sent.offset_freq, mode: MODE[t.sent.mod] ?? t.sent.mod, offset_freq: t.sent.offset_freq, low_cut: t.sent.low_cut, high_cut: t.sent.high_cut}
        : null,
  };
}

export function classify(url, frame) {
  if (!/\/ws\/?$/.test(new URL(url).pathname)) return 'other';
  if (typeof frame === 'string') return 'control';
  switch (frame[0]) {
    case 1:
    case 3:
      return 'waterfall';
    case 2:
    case 4:
      return 'audio';
    default:
      return 'other';
  }
}
