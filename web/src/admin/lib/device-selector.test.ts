import { describe, expect, it } from 'vitest';
import { deviceSelector } from './device-selector';

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
