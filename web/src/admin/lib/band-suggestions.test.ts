import { describe, expect, it } from 'vitest';
import { bandId, bandSection, dayAndNight, everything, fit, freeId, suggestionsFor, targetsFrom, type Tuning } from './band-suggestions';

const rtl: Tuning = { ranges: [[500000, 1766000000]], rates: [2400000, 2048000, 1024000], signal: 'iq' };
const rx888: Tuning = { ranges: [[0, 64800000]], rates: [64800000, 129600000], signal: 'real' };
const sdrplay: Tuning = { ranges: [[1000, 2000000000]], rates: [2000000, 6000000, 10000000], signal: 'iq' };

describe('band suggestions', () => {
  it('makes band ids the receiver takes', () => {
    expect(bandId('40 m')).toBe('40m');
    expect(bandId('Airband (AM)')).toBe('airband-am');
    expect(bandId('31 m broadcast')).toBe('31m-broadcast');
    expect(freeId('40m', ['40m', '40m-2'])).toBe('40m-3');
    expect(freeId('20m', ['40m'])).toBe('20m');
  });

  it('shows a whole band with the smallest rate that holds it', () => {
    // 40 m in Region 1 is 200 kHz: the smallest rate, centred.
    const forty = fit(rtl, { name: '40 m', group: 'amateur', low: 7000000, high: 7200000 })!;
    expect(forty).toMatchObject({ id: '40m', rate: 1024000, center: 7100000, whole: true, low: 7000000, high: 7200000 });
    // 10 m is 1.7 MHz: 2.4 Msps shows 1.92 MHz of it, all of it.
    const ten = fit(rtl, { name: '10 m', group: 'amateur', low: 28000000, high: 29700000 })!;
    expect(ten).toMatchObject({ rate: 2400000, whole: true });
  });

  it('shows the bottom of a band too wide for the radio', () => {
    // 2 m in Region 2 is 4 MHz: an RTL-SDR shows the first 1.92 MHz, from 144.
    const two = fit(rtl, { name: '2 m', group: 'amateur', low: 144000000, high: 148000000 })!;
    expect(two.whole).toBe(false);
    expect(two.rate).toBe(2400000);
    expect(two.low).toBe(144000000);
    expect(two.center).toBe(144960000);
    // An RSP at 6 Msps takes all of it.
    expect(fit(sdrplay, { name: '2 m', group: 'amateur', low: 144000000, high: 148000000 })).toMatchObject({ rate: 6000000, whole: true });
  });

  it('offers nothing where the radio does not tune', () => {
    expect(fit(rtl, { name: '2200 m', group: 'amateur', low: 135700, high: 137800 })).toBeNull();
    expect(fit(rx888, { name: '40 m', group: 'amateur', low: 7000000, high: 7200000 })).toBeNull();
  });

  it('offers a real-signal radio everything from 0 Hz, once per rate', () => {
    const all = everything(rx888);
    expect(all.map((s) => s.name)).toEqual(['Shortwave, 0 to 30 MHz', '0 to 60 MHz, with 6 m']);
    expect(all[0]).toMatchObject({ center: 0, rate: 64800000, signal: 'real', low: 0, high: 30000000 });
    expect(suggestionsFor(rx888, [{ name: '40 m', group: 'amateur', low: 7000000, high: 7200000 }])).toHaveLength(2);
  });

  it('offers a wide IQ radio shortwave from 0 Hz in one band', () => {
    const all = everything(sdrplay);
    expect(all.map((s) => s.name)).toEqual(['Shortwave, 0 to 5.8 MHz', 'Shortwave, 0 to 9.7 MHz']);
    // The centre misses WWV on 5 MHz, and the band starts where the input does.
    expect(all[1]).toMatchObject({ rate: 10000000, center: 5025000, signal: 'iq', low: 25000, high: 9700000 });
    const section = bandSection(all[1], 'sdrplay', '');
    expect(section.get('low')).toBe('25000');
    expect(section.get('high')).toBe('9700000');
    // An RTL-SDR does not tune that low, and 2.4 Msps is no shortwave band.
    expect(everything(rtl)).toEqual([]);
  });

  it('takes amateur bands and the services worth listening to from the plan', () => {
    const targets = targetsFrom(
      [{ name: '40 m', low: 7000000, high: 7200000 }],
      [
        { label: '41 m broadcast', low: 7200000, high: 7450000, layer: 'service' },
        { label: 'Airband (AM)', low: 117975000, high: 137000000, layer: 'service' },
        { label: 'Airband (AM)', low: 117975000, high: 137000000, layer: 'service' },
        { label: 'NDB beacons', low: 190000, high: 415000, layer: 'service' },
        { label: '40 m, CW', low: 7000000, high: 7040000, layer: 'amateur' },
      ],
    );
    expect(targets.map((t) => `${t.group}:${t.name}`)).toEqual(['amateur:40 m', 'broadcast:41 m broadcast', 'aviation:Airband']);
    const suggested = suggestionsFor(rtl, targets);
    expect(suggested.map((s) => s.name)).toEqual(['40 m', '41 m broadcast', 'Airband']);
  });

  it('puts the lower band at night when two share a radio', () => {
    const forty = fit(rtl, { name: '40 m', group: 'amateur', low: 7000000, high: 7200000 })!;
    const twenty = fit(rtl, { name: '20 m', group: 'amateur', low: 14000000, high: 14350000 })!;
    expect(dayAndNight(forty, twenty)).toEqual(['sunset-sunrise', 'sunrise-sunset']);
    expect(dayAndNight(twenty, forty)).toEqual(['sunrise-sunset', 'sunset-sunrise']);
  });

  it('writes the configuration section a band needs', () => {
    const forty = fit(rtl, { name: '40 m', group: 'amateur', low: 7000000, high: 7200000 })!;
    expect([...bandSection(forty, 'rtlsdr', 'serial:00000001', 'sunset-sunrise')]).toEqual([
      ['name', '40 m'], ['source', 'module'], ['module', 'rtlsdr'], ['sample_rate', '1024000'], ['center', '7100000'],
      ['signal', 'iq'], ['module.device', 'serial:00000001'], ['dc_remove', 'yes'], ['hours', 'sunset-sunrise'],
    ]);
    const hf = bandSection(everything(rx888)[0], 'rx888', '');
    expect(hf.get('signal')).toBe('real');
    expect(hf.get('high')).toBe('30000000');
    expect(hf.has('module.device')).toBe(false);
  });
});
