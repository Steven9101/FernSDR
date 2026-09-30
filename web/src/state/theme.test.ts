import { describe, expect, it } from 'vitest';
import { safeUrl } from './theme';

describe('safeUrl', () => {
  it('keeps the addresses a url() may hold', () => {
    expect(safeUrl('https://example.org/a.jpg')).toBe('https://example.org/a.jpg');
    expect(safeUrl('/uploads/a.png')).toBe('/uploads/a.png');
    expect(safeUrl('javascript:alert(1)')).toBe('');
  });

  it('ignores a value that is not a string rather than throwing', () => {
    for (const value of [1, true, {}, ['/a.png'], null, undefined]) {
      expect(safeUrl(value)).toBe('');
    }
  });
});
