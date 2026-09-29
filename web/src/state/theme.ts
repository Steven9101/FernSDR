/**
 * Applying the operator's theme.
 *
 * The receiver ships with a monochrome look; an operator can repaint any of it
 * from the admin panel, and the change lands on pages that are already open.
 * That last part is why this exists as a function that mutates the document
 * rather than as component state: a theme change touches the whole page, and
 * threading it through every component would be a lot of plumbing to make one
 * rarely-used feature work.
 *
 * The rule the whole design rests on: a theme sets CSS custom properties, and
 * nothing else. It cannot inject markup, styles or script. The server has
 * already checked that every colour is a hex value and every URL has a scheme
 * we are willing to put in a `url()`, and this does not undo that by building
 * CSS out of anything it was not given.
 */

export interface ThemeBackground {
  image: string;
  opacity: number;
  blur: number;
  position?: string;
}

export interface ThemeWidget {
  type: 'chat' | 'lightning' | 'clock' | 'notice' | 'links' | 'image' | 'embed' | 'spots' | 'space' | 'greyline' | 'station';
  title?: string;
  url?: string;
  text?: string;
  items?: { label: string; url: string }[];
  height?: number;
}

export interface Theme {
  name?: string;
  colors?: Record<string, string>;
  background?: ThemeBackground;
  logo?: string;
  palette?: string;
  /**
   * Which signal meter listeners see. The bar is the default and what this
   * receiver has always shown; the rest read the same calibrated value.
   */
  meter?: 'bar' | 'needle' | 'numeric' | 'history';
  widgets?: ThemeWidget[];
}

/**
 * Theme keys are camelCase; the CSS properties they set are kebab-case, and a
 * few do not match at all. Written out rather than derived, so that adding a
 * token is a deliberate act in two places instead of an accident in one.
 */
const CSS_PROPERTY: Record<string, string> = {
  background: '--background',
  card: '--card',
  popover: '--popover',
  muted: '--muted',
  elevated: '--elevated',
  foreground: '--foreground',
  mutedForeground: '--muted-foreground',
  subtleForeground: '--subtle-foreground',
  border: '--border',
  borderStrong: '--border-strong',
  primary: '--primary',
  primaryForeground: '--primary-foreground',
  signal: '--signal',
  warning: '--warning',
  destructive: '--destructive',
  success: '--success',
};

/** Only these are ever written into a url(); anything else is ignored. */
function safeUrl(value: string | undefined): string {
  if (!value) return '';
  if (/[()"'\\\s]/.test(value)) return '';
  if (/^https?:\/\//i.test(value) || value.startsWith('/') || /^data:image\//i.test(value)) {
    return value;
  }
  return '';
}

let styleElement: HTMLStyleElement | null = null;

export function applyTheme(theme: Theme | null | undefined): void {
  const root = document.documentElement;

  // Clear whatever a previous theme set, so removing a colour in the admin
  // panel actually removes it rather than leaving the last value stuck.
  for (const property of Object.values(CSS_PROPERTY)) root.style.removeProperty(property);

  if (theme?.colors) {
    for (const [key, value] of Object.entries(theme.colors)) {
      const property = CSS_PROPERTY[key];
      // Re-checked here, not because the server is untrusted, but because this
      // is the last point before the value becomes CSS and the check is one
      // regular expression.
      if (property && /^#[0-9a-fA-F]{3,8}$/.test(value)) root.style.setProperty(property, value);
    }
  }

  const background = theme?.background;
  const image = safeUrl(background?.image);
  if (!styleElement) {
    styleElement = document.createElement('style');
    styleElement.id = 'fernsdr-theme';
    document.head.appendChild(styleElement);
  }

  if (image) {
    const opacity = Math.min(1, Math.max(0, background?.opacity ?? 0.35));
    const blur = Math.min(40, Math.max(0, background?.blur ?? 0));
    const size = background?.position === 'tile' ? 'auto' : 'cover';
    const repeat = background?.position === 'tile' ? 'repeat' : 'no-repeat';
    // A fixed layer behind everything rather than a body background: the app is
    // a full-height grid of opaque panels, and painting behind it means the
    // panels have to become translucent, which is what the second rule does.
    styleElement.textContent = `
      body::before {
        content: '';
        position: fixed;
        inset: 0;
        z-index: -1;
        background-image: url("${image}");
        background-size: ${size};
        background-position: center;
        background-repeat: ${repeat};
        opacity: ${opacity};
        ${blur > 0 ? `filter: blur(${blur}px);` : ''}
        pointer-events: none;
      }
      .app, .topbar, .sidebar, .sheet, .status { background-color: transparent; }
      .topbar, .sidebar, .sheet, .status {
        backdrop-filter: blur(12px);
        background-color: color-mix(in srgb, var(--card) 72%, transparent);
      }
    `;
  } else {
    styleElement.textContent = '';
  }
}
