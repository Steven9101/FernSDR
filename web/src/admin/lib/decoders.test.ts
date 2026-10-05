import { describe, expect, it } from 'vitest';
import { decoderSection, formatDial, parseChannels, parseFrequency, suggestedChannels, validDecoderId, withModuleSettings } from './decoders';

const bands = [
  { id: '40m', name: '40 m', low: 7_000_000, high: 7_200_000 },
  { id: '20m', name: '20 m', low: 14_070_000, high: 14_076_000 },
  { id: '80m', name: '80 m', low: 3_500_000, high: 3_574_000 },
];

describe('decoder channels', () => {
  it('offers the dials a band can hear with room for the channel', () => {
    expect(suggestedChannels(bands, 'ft8')).toEqual([{ band: '40m', dial: 7_074_000 }]);
    expect(suggestedChannels(bands, 'ft8', 2000)).toEqual([{ band: '40m', dial: 7_074_000 }, { band: '20m', dial: 14_074_000 }]);
    expect(suggestedChannels(bands, 'ft4').map((c) => c.dial)).toEqual([7_047_500]);
    expect(suggestedChannels(bands, 'sstv')).toEqual([]);
  });

  it('reads frequencies and channel lists as the receiver does', () => {
    expect(parseFrequency('7074000')).toBe(7_074_000);
    expect(parseFrequency('7.074M')).toBe(7_074_000);
    expect(parseFrequency('7047.5k')).toBe(7_047_500);
    expect(parseFrequency('14074 kHz')).toBe(14_074_000);
    expect(parseFrequency('7.074 MHz')).toBe(7_074_000);
    expect(parseFrequency('-5')).toBeNull();
    expect(parseFrequency('7x')).toBeNull();
    expect(parseChannels('40m:7074000, 20m:14.074M')).toEqual([{ band: '40m', dial: 7_074_000 }, { band: '20m', dial: 14_074_000 }]);
    expect(parseChannels('7074000')).toBeNull();
    expect(parseChannels('')).toEqual([]);
  });

  it('writes the section and keeps what the form does not own', () => {
    const previous = new Map([['module', 'old'], ['mode', 'ft4'], ['module.depth', '3'], ['public', 'yes']]);
    const section = decoderSection({ module: 'ft8', channels: [{ band: '40m', dial: 7_047_500 }], public: false }, previous);
    expect([...section]).toEqual([['module', 'ft8'], ['channels', '40m:7047500'], ['public', 'no'], ['mode', 'ft4'], ['module.depth', '3']]);
  });

  it('writes the mode it is given, for a module that decodes more than one', () => {
    const section = decoderSection({ module: 'ft8', mode: 'ft4', channels: [{ band: '40m', dial: 7_047_500 }], public: false }, null);
    expect([...section]).toEqual([['module', 'ft8'], ['mode', 'ft4'], ['channels', '40m:7047500'], ['public', 'no']]);
    const kept = decoderSection({ module: 'ft8', mode: 'ft8', channels: [], public: true }, new Map([['mode', 'ft4']]));
    expect(kept.get('mode')).toBe('ft8');
  });

  it('names things plainly', () => {
    expect(formatDial(7_074_000)).toBe('7,074 kHz');
    expect(formatDial(7_047_500)).toBe('7,047.5 kHz');
    expect(validDecoderId('ft8-40m')).toBe(true);
    expect(validDecoderId('FT8')).toBe(false);
    expect(validDecoderId('')).toBe(false);
  });
});

describe('module settings in a decoder section', () => {
  const depth = { key: 'depth', type: 'number' as const, label: 'Depth', min: 1, max: 3, default: 2 };
  it('writes what differs from the default and leaves the rest out', () => {
    const base = new Map([['module', 'ft8'], ['module.depth', '1'], ['module.gone', 'x']]);
    expect([...withModuleSettings(base, [depth], { depth: '3' })]).toEqual([['module', 'ft8'], ['module.depth', '3'], ['module.gone', 'x']]);
    expect(withModuleSettings(base, [depth], { depth: '2' }).has('module.depth')).toBe(false);
    expect(withModuleSettings(base, [depth], {}).has('module.depth')).toBe(false);
  });
  it('refuses a value the setting cannot take', () => {
    expect(() => withModuleSettings(new Map(), [depth], { depth: '4' })).toThrow('Depth goes from 1 to 3');
    expect(() => withModuleSettings(new Map(), [depth], { depth: 'two' })).toThrow();
    const mode = { key: 'mode', type: 'choice' as const, label: 'Mode', choices: ['a', 'b'] };
    expect(() => withModuleSettings(new Map(), [mode], { mode: 'c' })).toThrow('Mode is one of a, b');
    expect(withModuleSettings(new Map(), [mode], { mode: 'b' }).get('module.mode')).toBe('b');
  });
});
