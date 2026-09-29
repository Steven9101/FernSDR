import { expect, it } from 'vitest';
import { BitReader } from './bitio';

it('reads unsigned fields across byte boundaries and zero-pads truncated fields', () => {
  const data = Uint8Array.of(0x8f, 0xe1, 0xa5, 0x73, 0xcd);
  const binary = [...data].map(byte => byte.toString(2).padStart(8, '0')).join('');
  for (let start = 0; start <= binary.length; start++) {
    for (let count = 0; count <= 32; count++) {
      const reader = new BitReader(data);
      for (let i = 0; i < start; i++) reader.bit();
      const field = binary.slice(start, start + count).padEnd(count, '0');
      expect(reader.bits(count)).toBe(field ? Number.parseInt(field, 2) : 0);
      expect(reader.bitsRead).toBe(Math.min(binary.length, start + count));
      expect(reader.overrun).toBe(start + count > binary.length);
    }
  }
});

it('keeps the end position after repeated empty reads, including 32 bits', () => {
  const reader = new BitReader(Uint8Array.of(0xff, 0xff, 0xff, 0xff));
  expect(reader.bits(32)).toBe(0xffffffff);
  expect(reader.overrun).toBe(false);
  for (const count of [0, 1, 8, 31, 32]) {
    expect(reader.bits(count)).toBe(0);
    expect(reader.bitsRead).toBe(32);
    expect(reader.overrun).toBe(count !== 0);
  }
});
