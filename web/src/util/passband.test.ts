import { describe, expect, it } from 'vitest';
import { presetPassband, constrainPassband, displayPassband, passbandShift, shiftedPassband } from './passband';
import { carrierForSignal } from './cw';

describe('CW filter presets', () => {
  it('holds the opposite edge still at channel and width limits', () => {
    expect(constrainPassband(300, 9000, -6000, 6000, 12000, 'high')).toEqual({ low: 300, high: 6000 });
    expect(constrainPassband(-9000, 2000, -6000, 6000, 4000, 'low')).toEqual({ low: -2000, high: 2000 });
    expect(constrainPassband(2200, 2000, -6000, 6000, 4000, 'low')).toEqual({ low: 1950, high: 2000 });
  });
  for (const mode of ['cw', 'cwl']) for (const pitch of [200, 700, 900, 1500]) {
    it(`keeps ${mode} on the signal at pitch ${pitch}`, () => {
      const signal = 14_050_000;
      const carrier = carrierForSignal(signal, mode, pitch);
      for (const width of [500, 250, 100, 1000, 500]) {
        const filter = presetPassband(mode, pitch, -width / 2, width / 2);
        expect(filter.high - filter.low).toBe(width);
        expect(carrier + (filter.low + filter.high) / 2).toBe(signal);
      }
    });
  }
});

describe('passband as the operator reads it', () => {
  it('shows SSB as audio edges on either side and CW around the note', () => {
    expect(displayPassband('lsb', 700, -2700, -300)).toEqual({ low: 300, high: 2700 });
    expect(displayPassband('usb', 700, 300, 2700)).toEqual({ low: 300, high: 2700 });
    const cw = presetPassband('cw', 700, -250, 250);
    expect(displayPassband('cw', 700, cw.low, cw.high)).toEqual({ low: -250, high: 250 });
    const shown = displayPassband('cwl', 600, presetPassband('cwl', 600, -100, 150).low, presetPassband('cwl', 600, -100, 150).high);
    expect(shown).toEqual({ low: -100, high: 150 });
  });

  it('shifts keeping the width, and reads the shift back', () => {
    const up = shiftedPassband('usb', 700, 300, 2700, 200);
    expect(up).toEqual({ low: 500, high: 2900 });
    expect(passbandShift('usb', 700, up.low, up.high)).toBe(200);
    // LSB shifts in audio terms too: up means higher tones.
    const lsb = shiftedPassband('lsb', 700, -2700, -300, 200);
    expect(displayPassband('lsb', 700, lsb.low, lsb.high)).toEqual({ low: 500, high: 2900 });
    expect(passbandShift('lsb', 700, lsb.low, lsb.high)).toBe(200);
    const am = shiftedPassband('am', 700, -3000, 3000, -500);
    expect(am).toEqual({ low: -3500, high: 2500 });
    expect(passbandShift('am', 700, -4500, 4500)).toBe(0);
  });
});
