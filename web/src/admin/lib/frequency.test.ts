import { describe, expect, it } from 'vitest';
import { configNumber, correctedPpm, pendingCorrection, summarise } from './frequency';

describe('correctedPpm', () => {
  it('finds the error of a crystal running fast', () => {
    // Fast by 50 ppm: everything shows low by that factor.
    expect(correctedPpm(10e6, 10e6 / (1 + 50e-6), 0, 0)).toBeCloseTo(50, 6);
  });

  it('refines a correction already in force rather than replacing it', () => {
    const shown = (10e6 / (1 + 50e-6)) * (1 + 30e-6);
    expect(correctedPpm(10e6, shown, 30, 0)).toBeCloseTo(50, 6);
  });

  it('leaves an upconverter offset out of the scaling', () => {
    // A 125 MHz converter: the radio tunes 135 MHz for 10 MHz; its crystal is off by -12 ppm.
    const offset = -125e6;
    const shown = (10e6 - offset) / (1 - 12e-6) + offset;
    expect(correctedPpm(10e6, shown, 0, offset)).toBeCloseTo(-12, 5);
  });
});

describe('summarise', () => {
  it('gives the mean and the spread', () => {
    const { mean, spread } = summarise([10, 12, 14]);
    expect(mean).toBe(12);
    expect(spread).toBeCloseTo(Math.sqrt(8 / 3), 10);
  });
});

describe('pendingCorrection', () => {
  const file = (lines: string) => `[band:hf]\nsource = test\n${lines}[band:vhf]\nppm = 40\n`;

  it('lets a correction be written when the file says what the receiver runs with', () => {
    expect(pendingCorrection(file(''), 'hf', { ppm: 0, offset: 0 })).toBeNull();
    expect(pendingCorrection(file('ppm = 1.25 # measured\n'), 'hf', { ppm: 1.25, offset: 0 })).toBeNull();
    expect(pendingCorrection(file('frequency_offset = -125e6\n'), 'hf', { ppm: 0, offset: -125e6 })).toBeNull();
  });

  it('refuses while a written correction waits for a restart', () => {
    const waiting = pendingCorrection(file('ppm = 3.5\n'), 'hf', { ppm: 0, offset: 0 });
    expect(waiting).toContain('ppm = 3.5');
    expect(waiting).toContain('runs with 0.00');
    expect(pendingCorrection(file('frequency_offset = 12\n'), 'hf', { ppm: 0, offset: 0 })).toContain(
      'frequency_offset = 12'
    );
    // Taken out of the file but still in force is waiting too.
    expect(pendingCorrection(file(''), 'hf', { ppm: 2, offset: 0 })).toContain('ppm = 0');
  });

  it('reads only the band being measured', () => {
    expect(pendingCorrection(file(''), 'vhf', { ppm: 0, offset: 0 })).toContain('ppm = 40');
    expect(pendingCorrection(file(''), 'hf', { ppm: 0, offset: 0 })).toBeNull();
  });

  it('reads a value as the server does', () => {
    expect(pendingCorrection(file('frequency_offset = -125M\n'), 'hf', { ppm: 0, offset: -125e6 })).toBeNull();
    // Unreadable, the server falls back to no correction; writing one replaces the value.
    expect(pendingCorrection(file('ppm = fast\n'), 'hf', { ppm: 0, offset: 0 })).toBeNull();
    // Held to what a crystal can be off by, so a restart would change nothing.
    expect(pendingCorrection(file('ppm = 900\n'), 'hf', { ppm: 500, offset: 0 })).toBeNull();
  });
});

describe('configNumber', () => {
  it('reads plain numbers and the units the configuration allows', () => {
    expect(configNumber('-3.25')).toBe(-3.25);
    expect(configNumber('7.1M')).toBeCloseTo(7.1e6, 3);
    expect(configNumber('14074 kHz')).toBeCloseTo(14074e3, 3);
    expect(configNumber('1e3')).toBe(1000);
    expect(configNumber('12 Hz')).toBe(12);
  });

  it('refuses what the server would not read', () => {
    expect(configNumber('')).toBeNull();
    expect(configNumber('fast')).toBeNull();
    expect(configNumber('5 ppm')).toBeNull();
  });
});
