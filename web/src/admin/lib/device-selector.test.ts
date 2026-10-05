import { describe, expect, it } from 'vitest';
import { deviceSelector, deviceTuning } from './device-selector';

const look = () => [
  { port: '1-1', module: 'rtlsdr' },
  { port: '1-2', module: 'rtlsdr' },
  { port: '2-1', module: 'airspyhf', serial: 'ABC' },
];

describe('device selector', () => {
  it('finds a radio chosen before the hardware was looked at again', () => {
    const before = look();
    const chosen = before[1];
    // "Look again": the same radios, described by new objects.
    expect(deviceSelector(look(), chosen)).toBe('index:1');
  });

  it('uses the serial where there is one, and nothing for the only radio of its module', () => {
    const radios = [...look(), { port: '2-2', module: 'airspyhf', serial: 'DEF' }];
    expect(deviceSelector(radios, { port: '2-2', module: 'airspyhf', serial: 'DEF' })).toBe('serial:DEF');
    expect(deviceSelector(look(), { port: '2-1', module: 'airspyhf' })).toBe('');
  });

  it('refuses a radio that is no longer there', () => {
    expect(deviceSelector(look(), { port: '3-1', module: 'rtlsdr' })).toBeNull();
  });
});

describe("the chosen radio's own tuning", () => {
  const r2 = { serial: '26D464DC2A8E7A2B', tuning: 'r2' };
  const mini = { serial: '0000000000000002', tuning: 'mini' };
  it('finds the radio by its serial, inside the text the kernel gives it', () => {
    expect(deviceTuning([r2, mini], { port: '1-2', module: 'airspy', serial: 'AIRSPY SN:26D464DC2A8E7A2B' })).toBe('r2');
    expect(deviceTuning([r2, mini], { port: '1-3', module: 'airspy', serial: '0000000000000002' })).toBe('mini');
  });
  it('takes the only device listed, and nothing where it cannot tell', () => {
    expect(deviceTuning([mini], { port: '1-3', module: 'airspy' })).toBe('mini');
    expect(deviceTuning([r2, mini], { port: '1-3', module: 'airspy' })).toBeUndefined();
    expect(deviceTuning([{ serial: 'X' }], { port: '1-3', module: 'airspy' })).toBeUndefined();
  });
});
