import { describe, expect, it } from 'vitest';
import { describeTuning, findSpots, segment } from './spots';

/** A receiver covering 40 m, 20 m and the 10 GHz beacon band through a transverter. */
const COVERAGE = [
  { low: 6_900_000, high: 7_400_000 },
  { low: 13_800_000, high: 14_600_000 },
  { low: 10_489_000_000, high: 10_490_000_000 },
];

const first = (text: string) => findSpots(text, COVERAGE)[0];

describe('finding frequencies in what people type', () => {
  it('reads a bare number the way it is said', () => {
    // A thousand and over is kHz, below it is MHz: both of these are 14.074.
    expect(first('14074')?.hz).toBe(14_074_000);
    expect(first('14.074')?.hz).toBe(14_074_000);
  });

  it('takes an explicit unit over the convention', () => {
    expect(first('14074 kHz')?.hz).toBe(14_074_000);
    expect(first('14.074 MHz')?.hz).toBe(14_074_000);
    expect(first('14074000 Hz')?.hz).toBe(14_074_000);
    expect(first('14.074MHz')?.hz).toBe(14_074_000);
  });

  it('takes a comma for a decimal point', () => {
    // Written by half of Europe, and this receiver's operators are there.
    expect(first('7,055')?.hz).toBe(7_055_000);
  });

  it('picks up the mode on either side', () => {
    expect(first('7055 lsb')?.mode).toBe('lsb');
    expect(first('LSB 7055')?.mode).toBe('lsb');
    expect(first('14200 USB')?.mode).toBe('usb');
    expect(first('14030 cw')?.mode).toBe('cw');
  });

  it('names the mode a digital signal is heard on', () => {
    // FT8 is not a mode the receiver has; USB is how you listen to it.
    expect(first('14074 FT8')?.mode).toBe('usb');
    expect(first('7047 js8')?.mode).toBe('usb');
  });

  it('leaves numbers that are not frequencies alone', () => {
    // The reason this rule exists: a chat window is full of these.
    expect(findSpots('73 and good luck', COVERAGE)).toEqual([]);
    expect(findSpots('599 in Berlin', COVERAGE)).toEqual([]);
    expect(findSpots('since 2019 or so', COVERAGE)).toEqual([]);
    expect(findSpots('at 21:30 tonight', COVERAGE)).toEqual([]);
  });

  it('leaves a time alone even when it lands in a band', () => {
    // "at 14.20 utc" is the middle of 20 m to a regular expression and twenty
    // past two to everybody else.
    expect(findSpots('sked at 14.20 utc', COVERAGE)).toEqual([]);
    expect(findSpots('back at 14.20 UTC tomorrow', COVERAGE)).toEqual([]);
    // But a mode still wins, because "14200 am" in a receiver's chat is AM.
    expect(first('14200 am')?.mode).toBe('am');
  });

  it('leaves numbers inside a link alone', () => {
    expect(findSpots('https://example.org/spots/14074', COVERAGE)).toEqual([]);
    expect(findSpots('see pskreporter.info/pskmap?f=14074', COVERAGE)).toEqual([]);
    // The same number outside one is still a frequency.
    expect(first('14074 on now')?.hz).toBe(14_074_000);
  });

  it('leaves frequencies this receiver cannot hear alone', () => {
    // Promising a link to somewhere it cannot tune would be a broken promise.
    expect(findSpots('3573 ft8 is busy', COVERAGE)).toEqual([]);
    expect(findSpots('145500 fm', COVERAGE)).toEqual([]);
  });

  it('finds a frequency behind a transverter', () => {
    expect(first('10489.540 MHz QO-100')?.hz).toBe(10_489_540_000);
  });

  it('finds more than one in a sentence', () => {
    const spots = findSpots('strong on 14074 and 7055 lsb tonight', COVERAGE);
    expect(spots.map((s) => s.hz)).toEqual([14_074_000, 7_055_000]);
    expect(spots[1].mode).toBe('lsb');
  });

  it('does not swallow the words around it', () => {
    const parts = segment('heard on 14074 just now', COVERAGE);
    expect(parts.map((p) => p.text).join('')).toBe('heard on 14074 just now');
    expect(parts.filter((p) => p.spot)).toHaveLength(1);
    expect(parts.find((p) => p.spot)?.text.trim()).toBe('14074');
  });

  it('rebuilds any message exactly', () => {
    // Whatever the parser does, the reader must see what was written.
    for (const text of [
      'nothing here',
      '14074',
      'usb 14200 and lsb 7055, both fine',
      '  14074  ',
      '14074 ft8 14074 ft8',
    ]) {
      expect(segment(text, COVERAGE).map((p) => p.text).join('')).toBe(text);
    }
  });

  it('finds nothing at all before the band list has arrived', () => {
    expect(findSpots('14074 usb', [])).toEqual([]);
  });

  it('writes a tuning the way it is quoted on the air', () => {
    expect(describeTuning(14_074_000, 'usb')).toBe('14074.000 kHz USB');
  });
});
