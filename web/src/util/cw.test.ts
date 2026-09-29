import { describe, expect, it } from 'vitest';

import { carrierForSignal, cwOffsetHz, signalForCarrier } from './cw';

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
