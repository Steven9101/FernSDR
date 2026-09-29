/*
The panel's places, in the address bar, so a link or a bookmark opens the band that needs looking
at and the back button goes back. Hash routes, because the receiver serves this page as a file:
there is no server-side router to hand a path to.
*/
import Gauge from '@lucide/svelte/icons/gauge';
import RadioTower from '@lucide/svelte/icons/radio-tower';
import Cpu from '@lucide/svelte/icons/cpu';
import Radar from '@lucide/svelte/icons/radar';
import ScrollText from '@lucide/svelte/icons/scroll-text';
import Antenna from '@lucide/svelte/icons/antenna';
import Palette from '@lucide/svelte/icons/palette';
import LayoutGrid from '@lucide/svelte/icons/layout-grid';
import FileCog from '@lucide/svelte/icons/file-cog';
import ArrowUpCircle from '@lucide/svelte/icons/arrow-up-circle';
import { tick } from 'svelte';

export const sections = [
  { id: 'overview', label: 'Overview', icon: Gauge },
  { id: 'bands', label: 'Bands', icon: RadioTower },
  { id: 'modules', label: 'Modules', icon: Cpu },
  { id: 'decoders', label: 'Decoders', icon: Radar },
  { id: 'log', label: 'Log', icon: ScrollText },
  { id: 'station', label: 'Station', icon: Antenna },
  { id: 'appearance', label: 'Appearance', icon: Palette },
  { id: 'widgets', label: 'Widgets', icon: LayoutGrid },
  { id: 'config', label: 'Configuration', icon: FileCog },
  { id: 'updates', label: 'Updates', icon: ArrowUpCircle },
] as const;

/** Places that are not in the navigation: the setup flow, reached from the overview. */
const hidden = ['setup'] as const;

export type Page = (typeof sections)[number]['id'] | (typeof hidden)[number];

export interface Route {
  page: Page;
  /** A band's id on its own page. */
  id?: string;
}

const pages = new Set<string>([...sections.map((section) => section.id), ...hidden]);

export function parseRoute(hash: string): Route {
  const [page, id] = hash.replace(/^#\/?/, '').split('/');
  if (!pages.has(page)) return { page: 'overview' };
  if (!id) return { page: page as Page };
  try {
    return { page: page as Page, id: decodeURIComponent(id) };
  } catch {
    // A link cut short mid-escape, such as #/bands/%: the section still opens.
    return { page: page as Page };
  }
}

export function href(route: Route): string {
  return `#/${route.page}${route.id ? `/${encodeURIComponent(route.id)}` : ''}`;
}

class Router {
  route = $state<Route>(parseRoute(location.hash));

  constructor() {
    window.addEventListener('hashchange', () => this.follow());
  }

  go(route: Route): void {
    const target = href(route);
    if (location.hash === target) return;
    location.hash = target;
  }

  private follow(): void {
    const next = parseRoute(location.hash);
    const previous = this.route;
    const kind = transitionKind(previous, next);
    // Only the page moves: the navigation keeps its own transition name and
    // stays put. Browsers without the API, and reduced motion, swap at once.
    const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches;
    if (!kind || reduced || !('startViewTransition' in document)) {
      this.route = next;
      return;
    }
    document.documentElement.dataset.transition = kind;
    const transition = document.startViewTransition(async () => {
      this.route = next;
      await tick();
    });
    void transition.finished.finally(() => {
      delete document.documentElement.dataset.transition;
    });
  }
}

function sectionIndex(page: Page): number {
  return sections.findIndex((section) => section.id === page);
}

function transitionKind(from: Route, to: Route): string | undefined {
  if (from.page === to.page && from.id === to.id) return undefined;
  if (from.page === to.page) return to.id ? 'forward' : 'back';
  return sectionIndex(to.page) > sectionIndex(from.page) ? 'tab-forward' : 'tab-back';
}

export const router = new Router();

/** Goes to a place in the panel, as a link there would. */
export function navigate(route: Route): void {
  router.go(route);
}
