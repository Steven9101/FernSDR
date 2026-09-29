import { beforeEach, describe, expect, it, vi } from 'vitest';
import { addLogEntry, adifBand, loadLogbook, logbook, MAX_LOG_ENTRIES, removeLogEntry, toAdif, toCsv } from './logbook';

const stored = new Map<string, string>();
beforeEach(() => {
  stored.clear();
  vi.stubGlobal('localStorage', { getItem: (k: string) => stored.get(k) ?? null, setItem: (k: string, v: string) => stored.set(k, v) });
  logbook.value = [];
});

const heard = Date.UTC(2026, 8, 28, 19, 5, 9);

describe('logbook', () => {
  it('keeps entries newest first, cleaned and bounded, across a reload', () => {
    addLogEntry({ time: heard, freq: 7_074_000, mode: 'usb', station: 'DL1ABC', report: '57', note: 'first' });
    addLogEntry({ time: heard + 60_000, freq: 9_420_000, mode: 'am', station: 'Radio\u0007 Somewhere', report: '', note: 'x'.repeat(500) });
    expect(addLogEntry({ time: heard, freq: -1, mode: 'usb', station: '', report: '', note: '' })).toBeNull();
    logbook.value = [];
    loadLogbook();
    expect(logbook.value.map((e) => e.freq)).toEqual([9_420_000, 7_074_000]);
    expect(logbook.value[0].station).toBe('Radio  Somewhere');
    expect(logbook.value[0].note).toHaveLength(200);
    removeLogEntry(logbook.value[0].id);
    expect(logbook.value).toHaveLength(1);
  });

  it('stops at the limit', () => {
    logbook.value = Array.from({ length: MAX_LOG_ENTRIES }, (_, i) => ({ id: `e${i}`, time: heard, freq: 7e6, mode: 'usb', station: '', report: '', note: '' }));
    expect(addLogEntry({ time: heard, freq: 7e6, mode: 'usb', station: '', report: '', note: '' })).toBeNull();
  });

  it('writes ADIF a logging program reads: counted fields, UTC, band, SWL', () => {
    addLogEntry({ time: heard, freq: 14_074_000, mode: 'usb', station: 'dl1abc/p', report: '59', note: 'Grüße' });
    addLogEntry({ time: heard, freq: 6_070_000, mode: 'dsb', station: '', report: '', note: '' });
    addLogEntry({ time: heard, freq: 9_610_000, mode: 'am', station: 'Radio Österreich', report: '', note: 'clear' });
    const adif = toAdif(logbook.value.slice().reverse(), heard);
    expect(adif).toContain('<ADIF_VER:5>3.1.4');
    expect(adif).toContain('<EOH>');
    expect(adif).toContain('<CALL:8>DL1ABC/P <QSO_DATE:8>20260928 <TIME_ON:6>190509 <FREQ:9>14.074000 <BAND:3>20m <MODE:3>SSB <SUBMODE:3>USB <RST_RCVD:2>59 <COMMENT:6>Grusse <SWL:1>Y <EOR>');
    // Outside the amateur bands and with no ADIF mode: those fields are left out.
    expect(adif).toContain('<FREQ:8>6.070000 <SWL:1>Y <EOR>');
    // A broadcaster's name is no callsign: it goes into the comment.
    expect(adif).toContain('<FREQ:8>9.610000 <MODE:2>AM <COMMENT:23>Radio Osterreich; clear <SWL:1>Y <EOR>');
    expect(adifBand(7_300_000)).toBe('40m');
    expect(adifBand(7_300_001)).toBeNull();
  });

  it('writes CSV that a spreadsheet cannot mistake for formulas', () => {
    addLogEntry({ time: heard, freq: 3_573_000, mode: 'cw', station: '=HYPERLINK("x")', report: '', note: 'said "hello"' });
    const csv = toCsv(logbook.value);
    expect(csv.split('\r\n')[1]).toBe('"2026-09-28 19:05:09","3573.000","CW","\'=HYPERLINK(""x"")","","said ""hello"""');
  });
});
