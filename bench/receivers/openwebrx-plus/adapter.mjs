// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Drives OpenWebRX+ 1.2.125's own page the way a listener does: the mode
// buttons and the frequency display in the receiver panel. The page keeps
// one WebSocket, /ws/, for everything; its JSON text messages are control,
// and a binary message's first byte says what it carries (htdocs/openwebrx.js,
// on_ws_recv): 1 waterfall, 2 audio, 3 the digital modes' secondary
// waterfall, 4 HD audio (WFM).
//
// Tuning never touches the profile list, and never sends `setfrequency`:
// both retune the SDR for every listener on it.
//
// CW differs from OpenWebRX: the dial shows the carrier, and the page sets
// the demodulator 800 Hz below it (UI.getCwOffset(): the middle of the CW
// passband, 700 to 900 Hz by default), so a carrier on the dial sounds at
// 800 Hz.

export const id = 'openwebrx-plus';

// Lab mode names to OpenWebRX+ modulations.
const MODULATION = {usb: 'usb', lsb: 'lsb', am: 'am', sam: 'sam', fm: 'nfm', cw: 'cw'};
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

// The dial the last control message stands for, in the page's convention:
// the demodulator's frequency, plus the CW tone offset in CW.
function controlDial(t) {
  if (!t || t.center === null || !('offset_freq' in t.sent)) return null;
  const s = t.sent;
  const tone = s.mod === 'cw' ? Math.round((s.low_cut + s.high_cut) / 2) : 0;
  return t.center + s.offset_freq + tone;
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
  if (!modulation) throw new Error(`OpenWebRX+ has no ${mode} mode`);
  const want = Math.round(freq);
  // Mode first: a new mode keeps the demodulator's offset, which in CW
  // moves the dial by the tone offset, so the dial is typed afterwards.
  await page.locator(`${PANEL} .openwebrx-demodulator-button[data-modulation="${modulation}"]`).click();
  // The dial is the page's own entry field: a click opens it, the unit
  // list switches it to Hz, Enter submits.
  const dial = page.locator(DIAL);
  await dial.click();
  await dial.locator('select').selectOption('0');
  await dial.locator('input').fill(String(want));
  await dial.locator('input').press('Enter');
  const t = traffic.get(page);
  await page.waitForFunction(
    ([dialSel, panel, wantFreq, wantMod]) => {
      const shown = $(dialSel).tuneableFrequencyDisplay().frequency;
      const on = document.querySelector(`${panel} .openwebrx-demodulator-button.highlighted[data-modulation]`);
      return shown === wantFreq && on && on.dataset.modulation === wantMod;
    },
    [DIAL, PANEL, want, modulation],
    {timeout: 10000},
  );
  // And the server has been told the same (the control message goes out
  // from the same call that updates the dial).
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (controlDial(t) === want && t.sent.mod === modulation) return;
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
  const control = controlDial(t);
  return {
    freq: shown.freq,
    mode: MODE[shown.modulation] ?? shown.modulation,
    shown: shown.text,
    // The dial implied by what the server was last told: its centre, the
    // offset, and in CW the tone offset.
    control:
      control === null
        ? null
        : {freq: control, mode: MODE[t.sent.mod] ?? t.sent.mod, offset_freq: t.sent.offset_freq, low_cut: t.sent.low_cut, high_cut: t.sent.high_cut},
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
