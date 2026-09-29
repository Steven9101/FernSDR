import { describe, expect, it } from 'vitest';
import { DRIVERS, type DriverId } from './drivers';

const session = (id: DriverId, address = 0x94) => DRIVERS.find((d) => d.id === id)!.create(address);
const text = (bytes: Uint8Array) => new TextDecoder().decode(bytes);
const hex = (bytes: Uint8Array) => [...bytes].map((b) => b.toString(16).padStart(2, '0')).join(' ');
const enc = (s: string) => new TextEncoder().encode(s);

describe('Kenwood, Elecraft, Flex', () => {
  it('asks, parses split answers, and sets with the digit count the radio uses', () => {
    const rig = session('kenwood');
    expect(rig.poll().map(text)).toEqual(['FA;', 'MD;']);
    expect(rig.parser.feed(enc('FA000140'))).toEqual([]);
    expect(rig.parser.feed(enc('74000;MD2;IF;?;'))).toEqual([{ freq: 14_074_000 }, { mode: 'usb' }]);
    expect(text(rig.setFrequency(7_030_500))).toBe('FA00007030500;');
    expect(text(rig.setMode('cwl')!)).toBe('MD7;');
    expect(text(rig.setMode('sam')!)).toBe('MD5;');
  });

  it('survives a wrong baud rate without growing', () => {
    const rig = session('kenwood');
    for (let i = 0; i < 100; i++) rig.parser.feed(new Uint8Array(64).fill(0x7f));
    expect(rig.parser.feed(enc(';FA00003573000;'))).toEqual([{ freq: 3_573_000 }]);
  });
});

describe('Yaesu new CAT', () => {
  it('learns an FT-950 style eight-digit dial', () => {
    const rig = session('yaesu');
    expect(rig.poll().map(text)).toEqual(['FA;', 'MD0;']);
    expect(text(rig.setFrequency(14_074_000))).toBe('FA014074000;');
    expect(rig.parser.feed(enc('FA14250000;MD0C;'))).toEqual([{ freq: 14_250_000 }, { mode: 'usb' }]);
    expect(text(rig.setFrequency(7_074_000))).toBe('FA07074000;');
    expect(text(rig.setMode('lsb')!)).toBe('MD01;');
  });
});

describe('Yaesu FT-817 family', () => {
  it('reads the five-byte answer and writes BCD in tens of hertz', () => {
    const rig = session('yaesu-classic');
    expect(hex(rig.poll()[0])).toBe('00 00 00 00 03');
    expect(rig.parser.feed(new Uint8Array([0x01, 0x40, 0x74, 0x00]))).toEqual([]);
    expect(rig.parser.feed(new Uint8Array([0x01]))).toEqual([{ freq: 14_074_000, mode: 'usb' }]);
    expect(hex(rig.setFrequency(7_030_000))).toBe('00 70 30 00 01');
    expect(hex(rig.setMode('nfm')!)).toBe('08 00 00 00 07');
    // A stray byte is dropped at the next poll instead of shifting every answer.
    rig.parser.feed(new Uint8Array([0x00]));
    rig.beforePoll!();
    expect(rig.parser.feed(new Uint8Array([0x00, 0x35, 0x73, 0x00, 0x02]))).toEqual([{ freq: 3_573_000, mode: 'cw' }]);
  });
});

describe('Icom CI-V', () => {
  it('frames commands for the address and reads answers and transceive broadcasts', () => {
    const rig = session('icom', 0x94);
    expect(rig.poll().map(hex)).toEqual(['fe fe 94 e0 03 fd', 'fe fe 94 e0 04 fd']);
    expect(hex(rig.setFrequency(14_074_000))).toBe('fe fe 94 e0 05 00 40 07 14 00 fd');
    expect(hex(rig.setMode('cwl')!)).toBe('fe fe 94 e0 06 07 fd');
    const reports = rig.parser.feed(new Uint8Array([
      // Our own query echoed back on the shared line: ignored.
      0xfe, 0xfe, 0x94, 0xe0, 0x03, 0xfd,
      // The answer, split across reads.
      0xfe, 0xfe, 0xe0, 0x94, 0x03, 0x00, 0x40, 0x07,
    ]));
    expect(reports).toEqual([]);
    expect(rig.parser.feed(new Uint8Array([0x14, 0x00, 0xfd, 0xfe, 0xfe, 0xe0, 0x94, 0x04, 0x02, 0x01, 0xfd]))).toEqual([
      { freq: 14_074_000 },
      { mode: 'am' },
    ]);
    // A dial turned on the radio, broadcast to everyone (transceive).
    expect(rig.parser.feed(new Uint8Array([0xfe, 0xfe, 0x00, 0x94, 0x00, 0x00, 0x00, 0x05, 0x07, 0x00, 0xfd]))).toEqual([{ freq: 7_050_000 }]);
    // Another radio on the same line is not ours.
    expect(rig.parser.feed(new Uint8Array([0xfe, 0xfe, 0xe0, 0xa4, 0x03, 0x00, 0x00, 0x05, 0x07, 0x00, 0xfd]))).toEqual([]);
  });
});
