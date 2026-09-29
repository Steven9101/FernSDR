import { describe, expect, it } from 'vitest';
import { parseBinary } from './protocol';
import vectors from '../dsp/testdata/vectors.json';

const meterBytes = () => Uint8Array.from(vectors.meter.match(/../g)!.map(value => parseInt(value, 16)));

it('keeps the native-grid and predictor flags independent at every wire width', () => {
  const bytes = new Uint8Array(23);
  const view = new DataView(bytes.buffer);
  bytes[0] = 2;
  view.setFloat64(4, 900000, true);
  view.setFloat64(12, 900100, true);
  view.setUint16(2, 65535, true);
  for (const width of [2, 15, 16, 4096]) for (const flags of [4, 5, 6, 7, 12, 13, 14, 15]) {
    bytes[1] = flags;
    view.setUint16(20, width, true);
    expect(parseBinary(bytes.buffer)).toMatchObject({ kind: 'waterfall', nativeGrid: true,
      adaptive: !!(flags & 2), zeroRuns: !!(flags & 1), stepDb: flags & 8 ? 2 : 1, sequence: 65535, width });
  }
  for (const flags of [0, 1, 2, 3, 8, 16, 255]) {
    bytes[1] = flags;
    view.setUint16(20, 2, true);
    expect(parseBinary(bytes.buffer)).toBeNull();
  }
});

it('reads compact audio independently of mute and the configuration generation', () => {
  const bytes = Uint8Array.from([1, 0xa3, 255, 255, 0]);
  expect(parseBinary(bytes.buffer)).toMatchObject({ kind: 'audio', generation: 10, muted: true,
    compact: true, sequence: 65535 });
  bytes[1] |= 4;
  expect(parseBinary(bytes.buffer)).toMatchObject({ kind: 'audio', generation: 10, muted: true,
    compact: true, discontinuity: true, sequence: 65535 });
  bytes[1] |= 8;
  expect(parseBinary(bytes.buffer)).toBeNull();
});

describe('negotiated binary meter', () => {
  it('reads the C++ wire fixture with its signs, units and optional fields intact', () => {
    expect(parseBinary(meterBytes().buffer)).toEqual({ kind: 'meter', type: 'meter',
      dbfs: -103.2, gain_db: 42.7, squelch_open: true, squelch_statistic: 34.5,
      pll_locked: true, pll_offset: -123.4, audio_bps: 32000, waterfall_bps: 8000,
      waterfall_fps: 12.5, listeners: 1000 });
  });

  it('reads the CTCSS tone under its own flag, and nothing under unknown flags', () => {
    const bytes = meterBytes();
    bytes[1] = 16 | 1;
    new DataView(bytes.buffer).setInt32(8, 885, true);
    expect(parseBinary(bytes.buffer)).toMatchObject({ ctcss: 88.5, pll_offset: undefined, pll_locked: undefined });
    bytes[1] = 32;
    expect(parseBinary(bytes.buffer)).toBeNull();
  });

  it('does not turn absent readings into measured zeros', () => {
    const bytes = meterBytes();
    bytes[1] = 0;
    expect(parseBinary(bytes.buffer)).toMatchObject({ squelch_open: false,
      squelch_statistic: undefined, pll_locked: undefined, pll_offset: undefined });
  });

  it('rejects truncated, extended and unknown layouts', () => {
    const bytes = meterBytes();
    for (let size = 1; size < 26; size++) expect(parseBinary(bytes.slice(0, size).buffer)).toBeNull();
    expect(parseBinary(new Uint8Array([...bytes, 0]).buffer)).toBeNull();
    bytes[1] = 0x80;
    expect(parseBinary(bytes.buffer)).toBeNull();
  });
});
