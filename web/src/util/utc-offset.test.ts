import { describe, expect, it } from 'vitest';
import { utcOffsetLabel } from './utc-offset';

describe('UTC offset label', () => {
  it('writes whole hours as before, and other offsets in hours and minutes', () => {
    expect(utcOffsetLabel(0)).toBe('UTC');
    expect(utcOffsetLabel(120)).toBe('UTC+2');
    expect(utcOffsetLabel(-300)).toBe('UTC−5');
    expect(utcOffsetLabel(330)).toBe('UTC+5:30');
    // Nepal and the Chatham Islands, which a decimal hour made 5.8 and 12.8.
    expect(utcOffsetLabel(345)).toBe('UTC+5:45');
    expect(utcOffsetLabel(765)).toBe('UTC+12:45');
    expect(utcOffsetLabel(-210)).toBe('UTC−3:30');
  });
});
