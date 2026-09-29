/*
What the Appearance page edits: the receiver's colour tokens, grouped the way someone thinks
about a page rather than the way the stylesheet is ordered, and the receiver's own values for
each, so an unset colour can be shown as what listeners actually get.

An unset colour is meaningful: it follows the receiver's built-in value, in whichever of its
light and dark schemes a listener uses, including when the receiver's palette changes later.
*/
import type { Theme } from '../../state/theme';

export type { Theme };

export interface ColorToken {
  key: string;
  label: string;
  hint: string;
}

export const TOKEN_GROUPS: { title: string; tokens: ColorToken[] }[] = [
  {
    title: 'Surfaces',
    tokens: [
      { key: 'background', label: 'Page', hint: 'Behind everything' },
      { key: 'card', label: 'Panels', hint: 'Top bar, sidebar, sheets' },
      { key: 'muted', label: 'Wells', hint: 'Slider tracks, hovers' },
      { key: 'elevated', label: 'Selected', hint: 'The raised chip in a control' },
    ],
  },
  {
    title: 'Text',
    tokens: [
      { key: 'foreground', label: 'Primary', hint: 'Readouts and headings' },
      { key: 'mutedForeground', label: 'Secondary', hint: 'Labels and values' },
      { key: 'subtleForeground', label: 'Tertiary', hint: 'Hints and footnotes' },
    ],
  },
  {
    title: 'Lines',
    tokens: [
      { key: 'border', label: 'Hairline', hint: 'Between controls' },
      { key: 'borderStrong', label: 'Strong', hint: 'Edges that must be seen' },
    ],
  },
  {
    title: 'Meaning',
    tokens: [
      { key: 'primary', label: 'Accent', hint: 'Selection and focus' },
      { key: 'signal', label: 'Signal', hint: 'Carrier marker, meter peak' },
      { key: 'success', label: 'Good', hint: 'Connected' },
      { key: 'warning', label: 'Warning', hint: 'Degraded' },
      { key: 'destructive', label: 'Bad', hint: 'Failed, disconnect' },
    ],
  },
];

/** The receiver's built-in colours, from its tokens.css, for both schemes. */
export const RECEIVER_DEFAULTS: Record<'dark' | 'light', Record<string, string>> = {
  dark: {
    background: '#070707',
    card: '#131313',
    popover: '#161616',
    muted: '#171717',
    elevated: '#3d3d3d',
    foreground: '#f7f7f7',
    mutedForeground: '#a4a4a4',
    subtleForeground: '#808080',
    border: '#1d1d1d',
    borderStrong: '#2f2f2f',
    primary: '#f7f7f7',
    primaryForeground: '#070707',
    signal: '#ff8a4c',
    warning: '#e8b339',
    destructive: '#ef5f5f',
    success: '#4cc38a',
  },
  light: {
    background: '#fcfcfc',
    card: '#f3f3f3',
    popover: '#ffffff',
    muted: '#f0f0f0',
    elevated: '#ffffff',
    foreground: '#121212',
    mutedForeground: '#5b5b5b',
    subtleForeground: '#696969',
    border: '#e5e5e5',
    borderStrong: '#d4d4d4',
    primary: '#121212',
    primaryForeground: '#ffffff',
    signal: '#c2410c',
    warning: '#926a00',
    destructive: '#b3261e',
    success: '#1a7f43',
  },
};

export const PRESETS: { name: string; colors: Record<string, string> }[] = [
  { name: 'Built in', colors: {} },
  {
    name: 'Amber CRT',
    colors: {
      background: '#0d0b07', card: '#14110b', muted: '#1d1810', elevated: '#2a2317',
      foreground: '#f5e6c8', mutedForeground: '#b8a27a', subtleForeground: '#7d6d50',
      border: '#241d13', borderStrong: '#3a2f1e', primary: '#ffb347', signal: '#ff7b3d',
    },
  },
  {
    name: 'Deep sea',
    colors: {
      background: '#050d14', card: '#0a1620', popover: '#0f1d2a', muted: '#122333',
      elevated: '#1a3247', foreground: '#e2f1ff', mutedForeground: '#8fb3cc',
      subtleForeground: '#5d7d94', border: '#132534', borderStrong: '#1f3d54',
      primary: '#4cc2ff', signal: '#ffb454',
    },
  },
  {
    name: 'Paper',
    colors: {
      background: '#f4f1ea', card: '#fffdf8', muted: '#e9e4d9', elevated: '#ffffff',
      foreground: '#1c1a15', mutedForeground: '#57513f', subtleForeground: '#7d7561',
      border: '#ddd6c7', borderStrong: '#bdb4a0', primary: '#1c1a15', signal: '#b4530a',
    },
  },
];

/** Every colour a listener in `scheme` gets under `theme`: the theme's, else the receiver's. */
export function resolvedColors(theme: Theme, scheme: 'dark' | 'light'): Record<string, string> {
  return { ...RECEIVER_DEFAULTS[scheme], ...(theme.colors ?? {}) };
}

/** Whether the theme's colours are exactly a preset's. */
export function matchesPreset(colors: Record<string, string> | undefined, preset: Record<string, string>): boolean {
  const own = colors ?? {};
  const keys = new Set([...Object.keys(own), ...Object.keys(preset)]);
  for (const key of keys) if ((own[key] ?? '').toLowerCase() !== (preset[key] ?? '').toLowerCase()) return false;
  return true;
}

/** The same check the server and the receiver make before a value becomes CSS. */
export function isHexColor(value: string): boolean {
  return /^#[0-9a-fA-F]{3,8}$/.test(value);
}

/** Only these addresses are put in a url(), mirroring the receiver's own rule. */
export function safeImageUrl(value: string | undefined): string {
  if (!value) return '';
  if (/[()"'\\\s]/.test(value)) return '';
  if (/^https?:\/\//i.test(value) || value.startsWith('/') || /^data:image\//i.test(value)) return value;
  return '';
}
