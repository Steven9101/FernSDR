import { describe, expect, it } from 'vitest';
import { matchesPreset, PRESETS, RECEIVER_DEFAULTS, resolvedColors, safeImageUrl } from './theme';

describe('resolvedColors', () => {
  it('fills what the theme leaves unset from the scheme a listener uses', () => {
    const theme = { colors: { primary: '#ff0000' } };
    expect(resolvedColors(theme, 'dark').primary).toBe('#ff0000');
    expect(resolvedColors(theme, 'dark').background).toBe(RECEIVER_DEFAULTS.dark.background);
    expect(resolvedColors(theme, 'light').background).toBe(RECEIVER_DEFAULTS.light.background);
  });
});

describe('matchesPreset', () => {
  it('matches regardless of letter case and treats no colours as the built-in look', () => {
    const amber = PRESETS[1].colors;
    const upper = Object.fromEntries(Object.entries(amber).map(([key, value]) => [key, value.toUpperCase()]));
    expect(matchesPreset(upper, amber)).toBe(true);
    expect(matchesPreset(undefined, PRESETS[0].colors)).toBe(true);
    expect(matchesPreset({ ...amber, signal: '#000000' }, amber)).toBe(false);
  });
});

describe('safeImageUrl', () => {
  it('keeps web and uploaded addresses and drops anything that could break out of url()', () => {
    expect(safeImageUrl('https://example.org/a.png')).toBe('https://example.org/a.png');
    expect(safeImageUrl('/uploads/0123.png')).toBe('/uploads/0123.png');
    expect(safeImageUrl('javascript:alert(1)')).toBe('');
    expect(safeImageUrl('https://example.org/a.png") x')).toBe('');
  });
});

describe('RECEIVER_DEFAULTS', () => {
  // The preview draws unset colours with these, so they must be the receiver's own. Read from
  // its stylesheet rather than trusted: a colour changed there and not here would make the
  // preview show a receiver that does not exist.
  it('are the values in the receiver stylesheet', async () => {
    const { readFileSync } = await import('node:fs');
    const css = readFileSync(new URL('../../styles/tokens.css', import.meta.url), 'utf8');
    const block = (selector: string) => {
      const start = css.indexOf(`${selector} {`);
      return css.slice(start, css.indexOf('\n}', start));
    };
    const kebab = (key: string) => `--${key.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`)}`;
    for (const [scheme, selector] of [['dark', ':root'], ['light', ":root[data-color-scheme='light']"]] as const) {
      const text = block(selector);
      for (const [key, value] of Object.entries(RECEIVER_DEFAULTS[scheme])) {
        const match = new RegExp(`${kebab(key)}:\\s*(#[0-9a-fA-F]+)`).exec(text);
        expect(match?.[1], `${scheme} ${key}`).toBe(value);
      }
    }
  });
});
