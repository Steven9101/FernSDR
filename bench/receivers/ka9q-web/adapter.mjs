// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Drives ka9q-web's own page (html/radio.html and radio.js at 4efea4b) as a
// listener does: the "Start Audio" button, the Mode selector and the
// frequency box, which takes kHz and tunes on Enter (its onchange handler,
// setFrequencyW()).
//
// The page only accepts frequencies from 0 to half the front end's sample
// rate (spectrum.js checkFrequencyIsValid(), and the server clamps the
// spectrum to the same range in check_frequency()): it assumes a real,
// direct-sampling front end like the RX888. On a complex front end centred
// away from 0 Hz, such as the lab's 2.048 Msps band at 7.1 MHz, a typed
// frequency above 1.024 MHz is silently ignored; readback() then shows the
// frequency the receiver really has, not the one typed.
//
// CW is radiod's cwu preset: a carrier at the dial sounds at +500 Hz.

export const id = 'ka9q-web';

const MODES = {usb: 'usb', lsb: 'lsb', am: 'am', sam: 'sam', fm: 'fm', cw: 'cwu'};
const LAB_MODES = Object.fromEntries(Object.entries(MODES).map(([lab, page]) => [page, lab]));

export async function open(page, baseUrl) {
  // "/" is a meta refresh to radio.html.
  await page.goto(baseUrl, {waitUntil: 'domcontentloaded'});
  await page.waitForURL(/radio\.html/, {timeout: 15000});
  // The page is usable once its WebSocket has its SSRC and the first
  // spectrum frame has told it the front end's sample rate.
  await page.waitForFunction(() => Number.isFinite(window.ssrc) && window.input_samprate > 0, null, {timeout: 30000});
}

export async function startAudio(page) {
  const button = page.locator('#audio_button');
  if ((await button.getAttribute('value')) === 'STOP') return; // already playing
  await button.click();
  await page.waitForFunction(() => document.getElementById('audio_button').value === 'STOP', null, {timeout: 5000});
}

export async function tune(page, {freq, mode}) {
  const value = MODES[mode];
  if (!value) throw new Error(`ka9q-web: no mode ${mode}`);
  // Mode first: changing to or from CW moves the frequency shown, so the
  // frequency typed afterwards is the one that stands.
  await page.selectOption('#mode', value);
  await page.waitForTimeout(300);
  const box = page.locator('#freq');
  await box.fill((freq / 1000).toFixed(3));
  await box.press('Enter');
  // The server confirms a frequency in its spectrum frames and BFREQ
  // messages; wait for it, but a frequency the page refuses never arrives.
  await page
    .waitForFunction((hz) => Math.abs(window.backendFrequencyHz - hz) <= 1, Math.round(freq), {timeout: 5000})
    .catch(() => {});
}

export async function readback(page) {
  return page.evaluate((labModes) => {
    const m = document.getElementById('mode').value;
    return {freq: Math.round(window.backendFrequencyHz), mode: labModes[m] ?? m};
  }, LAB_MODES);
}

// One WebSocket at "/" (ka9q-web.c). Text frames are commands and replies.
// Binary frames are RTP packets whose payload type says what they carry:
// 0x7F spectrum, 0x7E the channel's status; anything else is radiod's own
// audio packet (Opus or PCM) passed through.
export function classify(url, frame) {
  if (typeof frame === 'string') return 'control';
  if (!frame || frame.length < 12) return 'other';
  const type = frame[1] & 0x7f;
  if (type === 0x7f) return 'waterfall';
  if (type === 0x7e) return 'control';
  return 'audio';
}
