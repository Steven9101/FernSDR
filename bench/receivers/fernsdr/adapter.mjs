// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Drives FernSDR's own page (web/src at the pinned commit) as a listener
// does: the audio gate's button starts audio, the Mode buttons pick the
// mode, and "Type a frequency" takes the dial frequency.
//
// The page shows the signal's frequency, not the carrier's: in CW the
// receiver is tuned to the dial minus the pitch (700 Hz unless the listener
// changes it), so a carrier exactly at the dial sounds at 700 Hz
// (web/src/util/cw.ts). The URL fragment (#f=...&m=...) is read only when
// the page loads, so tuning goes through the frequency control instead.

export const id = 'fernsdr';

// The lab's mode names to the labels of the page's Mode buttons
// (web/src/components/ReceivePanel.svelte).
const MODE_LABELS = {usb: 'USB', lsb: 'LSB', cw: 'CW', cwl: 'CW-L', am: 'AM', sam: 'SAM', fm: 'NFM', dsb: 'DSB'};
const LABEL_MODES = Object.fromEntries(Object.entries(MODE_LABELS).map(([mode, label]) => [label, mode]));

const readout = (page) => page.getByRole('group', {name: /^Tuned to /});
const modeGroup = (page) => page.getByRole('radiogroup', {name: 'Mode', exact: true});

export async function open(page, baseUrl) {
  await page.goto(baseUrl, {waitUntil: 'domcontentloaded'});
  // The readout exists once the receiver's welcome has listed its bands.
  await readout(page).waitFor({timeout: 30000});
}

export async function startAudio(page) {
  // The gate stays until the AudioContext runs, which only a trusted gesture
  // may start; its button's accessible name is "Tap to listen".
  const gate = page.getByRole('status', {name: 'Start audio'});
  try {
    await gate.waitFor({state: 'visible', timeout: 5000});
  } catch {
    return; // no gate: audio is running already
  }
  await gate.getByRole('button').click();
  await gate.waitFor({state: 'detached', timeout: 15000});
}

export async function tune(page, {freq, mode}) {
  const label = MODE_LABELS[mode];
  if (!label) throw new Error(`fernsdr: no mode ${mode}`);
  // Receive is the first tab and open by default; after another tab, go back.
  if (!(await modeGroup(page).isVisible())) await page.getByRole('tab', {name: 'Receive', exact: true}).click();
  // Mode first: a change of mode keeps the frequency shown, so the dial
  // typed afterwards is the one that stands.
  await modeGroup(page).getByRole('radio', {name: label, exact: true}).click();
  await page.getByRole('button', {name: 'Type a frequency', exact: true}).click();
  const input = page.getByRole('textbox', {name: 'Frequency', exact: true});
  // With a unit the entry is exact; a bare number is guessed as MHz or kHz.
  await input.fill(`${Math.round(freq)}hz`);
  await input.press('Enter');
  await page.getByRole('group', {name: `Tuned to ${(freq / 1e6).toFixed(6)} megahertz`}).waitFor({timeout: 10000});
}

export async function readback(page) {
  const name = await readout(page).getAttribute('aria-label');
  const mhz = Number(/^Tuned to ([0-9.]+) megahertz$/.exec(name ?? '')?.[1]);
  const checked = await modeGroup(page).locator('[role="radio"][aria-checked="true"]').textContent();
  return {
    freq: Number.isFinite(mhz) ? Math.round(mhz * 1e6) : null,
    mode: LABEL_MODES[(checked ?? '').trim()] ?? (checked ?? '').trim().toLowerCase(),
  };
}

// One WebSocket, /ws, carries everything (docs/PROTOCOL.md): JSON control
// messages in text frames, and binary frames whose first byte says what they
// are (server/src/core/protocol.h): 1 audio, 2 waterfall, 3 the S-meter,
// counted as control since it is sent as JSON until the binary meter is
// negotiated.
export function classify(url, frame) {
  if (typeof frame === 'string') return 'control';
  if (!frame || frame.length === 0) return 'other';
  if (frame[0] === 0x01) return 'audio';
  if (frame[0] === 0x02) return 'waterfall';
  if (frame[0] === 0x03) return 'control';
  return 'other';
}
