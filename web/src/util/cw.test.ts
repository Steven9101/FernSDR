import { describe, expect, it } from 'vitest';

import { carrierForSignal, cwOffsetHz, edgesAtPitch, signalForCarrier } from './cw';

describe('cw offset', () => {
  it('leaves every non-CW mode alone', () => {
    for (const mode of ['usb', 'lsb', 'am', 'sam', 'nfm', 'dsb']) {
      expect(cwOffsetHz(mode, 700)).toBe(0);
      expect(carrierForSignal(14_050_000, mode, 700)).toBe(14_050_000);
    }
  });

  it('tunes below the signal for CW, so the signal lands in the passband', () => {
    // The server places the CW filter at {pitch-250, pitch+250} above the
    // carrier, so a signal is audible only when it is `pitch` above it.
    expect(carrierForSignal(14_050_000, 'cw', 700)).toBe(14_049_300);
    const passbandLow = 14_049_300 + 450;
    const passbandHigh = 14_049_300 + 950;
    expect(14_050_000).toBeGreaterThan(passbandLow);
    expect(14_050_000).toBeLessThan(passbandHigh);
  });

  it('mirrors for CW-L', () => {
    expect(carrierForSignal(14_050_000, 'cwl', 700)).toBe(14_050_700);
  });

  it('round-trips, so the readout shows what was clicked', () => {
    for (const mode of ['usb', 'cw', 'cwl']) {
      const carrier = carrierForSignal(7_030_000, mode, 600);
      expect(signalForCarrier(carrier, mode, 600)).toBe(7_030_000);
    }
  });
});

describe('CW edges kept at another pitch', () => {
  it('keeps a saved signal inside its saved filter when the pitch has changed since', () => {
    // Kept at pitch 900 with the filter 850 to 950 Hz, then restored by a
    // listener whose pitch is 700.
    const signal = 7_074_000;
    const carrier = carrierForSignal(signal, 'cw', 700);
    const edges = edgesAtPitch('cw', 850, 950, 900, 700);
    expect(signal - carrier).toBeGreaterThan(edges.low);
    expect(signal - carrier).toBeLessThan(edges.high);
    expect(edges).toEqual({ low: 650, high: 750 });
  });

  it('mirrors the move for CW-L and leaves entries without a kept pitch alone', () => {
    const signal = 7_074_000;
    const carrier = carrierForSignal(signal, 'cwl', 700);
    const edges = edgesAtPitch('cwl', -950, -850, 900, 700);
    expect(signal - carrier).toBeGreaterThan(edges.low);
    expect(signal - carrier).toBeLessThan(edges.high);
    expect(edgesAtPitch('cw', 850, 950, undefined, 700)).toEqual({ low: 850, high: 950 });
    expect(edgesAtPitch('usb', 300, 2700, 900, 700)).toEqual({ low: 300, high: 2700 });
  });
});
