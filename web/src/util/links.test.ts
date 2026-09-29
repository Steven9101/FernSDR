import { describe, expect, it } from 'vitest';
import { webLink } from './links';

describe('links', () => {
  it('opens web pages and pages on this receiver, nothing else', () => {
    expect(webLink('https://example.org/a')).toBe('https://example.org/a');
    expect(webLink('HTTP://example.org')).toBe('HTTP://example.org');
    expect(webLink('/uploads/a.png')).toBe('/uploads/a.png');
    for (const bad of ['javascript:alert(1)', ' javascript:alert(1)', 'data:text/html,x', '//evil.example', 'vbscript:x', '', undefined]) {
      expect(webLink(bad)).toBeUndefined();
    }
  });
});
