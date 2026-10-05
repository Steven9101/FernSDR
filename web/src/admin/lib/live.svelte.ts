/*
What the receiver is doing, fetched every few seconds while the panel is open and visible.

One poll for the whole panel rather than one per page: the navigation shows a band in trouble
whatever page is open, and a page opened later has the recent history for its sparklines instead
of starting empty. The history is kept here, in the browser, because only an open admin tab ever
looks at it and the receiver should not spend memory on it.
*/
import { api, ApiError, type AdminState } from '../api';
import { processorShare } from './machine';

const POLL_MS = 3000;
const HISTORY = 60;

export interface Series {
  listeners: number[];
  floor: number[];
  /** What the band's listeners are sent together, bits per second. */
  uplink: number[];
}

class Live {
  state = $state<AdminState | null>(null);
  error = $state('');
  updated = $state(0);
  unauthorised = $state(false);
  listeners = $state<number[]>([]);
  uplink = $state<number[]>([]);
  /** FernSDR's share of the CPU it may use, and the memory it holds. */
  processor = $state<number[]>([]);
  memory = $state<number[]>([]);
  bands = $state<Record<string, Series>>({});
  /** Per listener, their stream over the last minute. */
  streams = $state<Record<number, number[]>>({});

  private timer = 0;
  private running = false;
  private asked = 0;

  start(): void {
    if (this.running) return;
    this.running = true;
    document.addEventListener('visibilitychange', this.onVisibility);
    void this.refresh();
  }

  stop(): void {
    this.running = false;
    // An answer still on its way belongs to before the stop: a 401 in it
    // must not sign the panel out, as during a change of password.
    this.asked++;
    window.clearTimeout(this.timer);
    document.removeEventListener('visibilitychange', this.onVisibility);
  }

  /** Fetch now, and then on the usual schedule. */
  async refresh(): Promise<void> {
    window.clearTimeout(this.timer);
    // A refresh asked for by a page can overlap the poll; only the latest answer counts, so an
    // older one arriving late cannot put back a state the receiver has already left.
    const asked = ++this.asked;
    try {
      const next = await api.state();
      if (asked !== this.asked) return;
      this.record(next);
      this.state = next;
      this.error = '';
      this.updated = Date.now();
    } catch (problem) {
      if (asked !== this.asked) return;
      if ((problem as ApiError).status === 401) {
        this.unauthorised = true;
        this.stop();
        return;
      }
      this.error = (problem as Error).message;
    }
    if (this.running && !document.hidden) this.timer = window.setTimeout(() => void this.refresh(), POLL_MS);
  }

  private onVisibility = () => {
    // A tab in the background does not poll: nobody is looking, and a phone
    // left on the page should not keep its radio awake for it.
    if (document.hidden) window.clearTimeout(this.timer);
    else void this.refresh();
  };

  private record(next: AdminState): void {
    const push = (series: number[], value: number) => [...series, value].slice(-HISTORY);
    this.listeners = push(this.listeners, next.users);
    this.uplink = push(
      this.uplink,
      next.listeners.reduce((total, one) => total + one.audio_bitrate + one.waterfall_bitrate, 0),
    );
    if (next.machine) {
      this.processor = push(this.processor, processorShare(next.machine));
      this.memory = push(this.memory, next.machine.process_memory);
    }
    const bands: Record<string, Series> = {};
    for (const band of next.bands) {
      const before = this.bands[band.id] ?? { listeners: [], floor: [], uplink: [] };
      bands[band.id] = {
        listeners: push(before.listeners, band.listeners),
        floor: push(before.floor, band.noise_floor ?? -160),
        uplink: push(before.uplink, bandUplink(next, band.id)),
      };
    }
    this.bands = bands;
    const streams: Record<number, number[]> = {};
    for (const listener of next.listeners) {
      streams[listener.id] = push(this.streams[listener.id] ?? [], listener.audio_bitrate + listener.waterfall_bitrate);
    }
    this.streams = streams;
  }
}

export const live = new Live();

/** What one band's listeners are being sent together, bits per second. */
export function bandUplink(state: AdminState, band: string): number {
  return state.listeners
    .filter((listener) => listener.band === band)
    .reduce((total, listener) => total + listener.audio_bitrate + listener.waterfall_bitrate, 0);
}

/** A band's condition in one word, with the tone it is drawn in. */
export type BandCondition = 'receiving' | 'starting' | 'retrying' | 'needs-you' | 'off' | 'stopped' | 'restarting' | 'off-air';

export function bandCondition(band: {
  running: boolean;
  online?: boolean;
  restarting: boolean;
  status?: string;
  on_air?: boolean;
}): BandCondition {
  // Stopped by its hours, which is how it should be: said plainly, not as a fault.
  if (band.on_air === false) return 'off-air';
  if (band.restarting) return 'restarting';
  if (!band.running) return 'stopped';
  if (band.online ?? true) return 'receiving';
  const status = band.status ?? '';
  if (status.startsWith('switched off')) return 'off';
  if (status.startsWith('waiting for the operator')) return 'needs-you';
  if (status === 'starting') return 'starting';
  return 'retrying';
}

/**
 * Whether a band's condition is a fault worth the operator's attention. A
 * band off the air by its hours, or switched off, is doing what it was told.
 */
export function isTrouble(condition: BandCondition): boolean {
  return condition !== 'receiving' && condition !== 'off' && condition !== 'off-air';
}

/**
 * The receiver's state in a line when nothing needs the operator: every band
 * receiving, or how many are, and why the others are quiet. A day and night
 * pair has one band off the air at any time, and "all receiving" would say
 * otherwise.
 */
export function quietSummary(conditions: readonly BandCondition[]): string {
  const count = (which: BandCondition) => conditions.filter((condition) => condition === which).length;
  const receiving = count('receiving');
  if (receiving === conditions.length) return conditions.length === 1 ? 'Receiving' : `All ${conditions.length} bands receiving`;
  const parts = [`${receiving} receiving`];
  if (count('off-air')) parts.push(`${count('off-air')} off the air, as scheduled`);
  if (count('off')) parts.push(`${count('off')} switched off`);
  return parts.join(', ');
}

export const conditionLabel: Record<BandCondition, string> = {
  receiving: 'Receiving',
  starting: 'Starting',
  retrying: 'Retrying',
  'needs-you': 'Needs you',
  off: 'Switched off',
  stopped: 'Stopped',
  restarting: 'Restarting',
  'off-air': 'Off the air',
};

/** The reason, without the word the condition already says. */
export function conditionDetail(status: string | undefined): string {
  return (status ?? '')
    .replace(/^(offline|waiting for the operator|switched off|stopped): /, '')
    .replace(/^off the air (until|by)/, (_, word: string) => (word === 'until' ? 'Until' : 'By'))
    .replace(/ Trying again in \d+ s\.$/, '');
}
