/**
 * What the receiver's public decoders heard, read from /api/decodes while a
 * view of it is open.
 *
 * The first request takes the newest decodes; after that the page asks only
 * for what came after the last one it has, every few seconds, and not at all
 * while the tab is hidden: a listener who leaves the list open in a
 * background tab should cost the receiver nothing. On return the next request
 * catches up, up to the server's 2000.
 */
import { box } from './reactive.svelte';

export interface Decode {
  seq: number;
  decoder: string;
  channel: string;
  band: string;
  mode: string;
  /** UTC start of its slot, ms since 1970. */
  time: number;
  /** The channel's dial frequency and the signal's audio frequency above it, Hz. */
  dial: number;
  freq: number;
  snr: number;
  dt: number;
  message: string;
  quality: string;
  call?: string;
  grid?: string;
  report?: string;
}

/** Enough for an evening on a busy band, bounded so a long visit stays light. */
export const MAX_KEPT = 2000;
const POLL_MS = 5000;
const TIMEOUT_MS = 8000;

const text = (value: unknown, max: number) =>
  typeof value === 'string' && value.length <= max ? value : null;
const finite = (value: unknown) => (typeof value === 'number' && Number.isFinite(value) ? value : null);

/** One decode from the server, or null for anything malformed. */
export function validDecode(value: unknown): Decode | null {
  if (!value || typeof value !== 'object') return null;
  const v = value as Record<string, unknown>;
  const seq = finite(v.seq);
  const time = finite(v.time);
  const dial = finite(v.dial);
  const freq = finite(v.freq);
  const snr = finite(v.snr);
  const dt = finite(v.dt);
  const message = text(v.message, 64);
  const decoder = text(v.decoder, 64);
  const channel = text(v.channel, 64);
  const band = text(v.band, 64);
  const mode = text(v.mode, 16);
  const quality = text(v.quality, 8);
  if (seq === null || !Number.isSafeInteger(seq) || seq < 1 || time === null || dial === null || freq === null ||
      snr === null || dt === null || message === null || decoder === null || channel === null || band === null ||
      mode === null || quality === null) return null;
  const decode: Decode = { seq, decoder, channel, band, mode, time, dial, freq, snr, dt, message, quality };
  const call = text(v.call, 12);
  const grid = text(v.grid, 6);
  const report = text(v.report, 8);
  if (call) decode.call = call;
  if (grid) decode.grid = grid;
  if (report) decode.report = report;
  return decode;
}

export function parseDecodes(value: unknown): { epoch: string; through: number; decodes: Decode[] } | null {
  if (!value || typeof value !== 'object') return null;
  const v = value as Record<string, unknown>;
  const through = finite(v.through);
  if (through === null || !Number.isSafeInteger(through) || through < 0 || !Array.isArray(v.decodes)) return null;
  const decodes = v.decodes.map(validDecode).filter((d): d is Decode => d !== null);
  return { epoch: text(v.epoch, 64) ?? '', through, decodes };
}

/** `kept` with `incoming` added, in order, each once, and no more than `cap`. */
export function mergeDecodes(kept: Decode[], incoming: Decode[], cap = MAX_KEPT): Decode[] {
  if (incoming.length === 0) return kept;
  const last = kept.length ? kept[kept.length - 1].seq : 0;
  const fresh = incoming.filter((d) => d.seq > last);
  if (fresh.length === 0) return kept;
  const merged = kept.concat(fresh.sort((a, b) => a.seq - b.seq));
  return merged.length > cap ? merged.slice(merged.length - cap) : merged;
}

export interface DecodeFilter {
  /** A channel id, or '' for all of them. */
  channel: string;
  cqOnly: boolean;
  /** Matched against the message, upper case. */
  search: string;
}

export const isCq = (message: string) => /^CQ(\s|$)/.test(message);

export function filterDecodes(list: Decode[], filter: DecodeFilter): Decode[] {
  const search = filter.search.trim().toUpperCase();
  return list.filter((d) =>
    (!filter.channel || d.channel === filter.channel) &&
    (!filter.cqOnly || isCq(d.message)) &&
    (!search || d.message.includes(search)));
}

export type FeedStatus = 'idle' | 'loading' | 'live' | 'error';

export class DecodeFeed {
  readonly list = box<Decode[]>([]);
  readonly status = box<FeedStatus>('idle');
  private through: number | null = null;
  private epoch: string | null = null;
  private readers = 0;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private controller: AbortController | null = null;
  // Bumped when the last reader leaves, so a request still on its way from
  // before cannot start a second polling loop beside the new one.
  private generation = 0;

  constructor(private readonly load: (url: string, signal: AbortSignal) => Promise<unknown> = fetchJson) {}

  /** Starts following while anything reads; the returned function stops reading. */
  open(): () => void {
    this.readers++;
    if (this.readers === 1) void this.poll();
    let closed = false;
    return () => {
      if (closed) return;
      closed = true;
      this.readers--;
      if (this.readers === 0) this.halt();
    };
  }

  private halt() {
    this.generation++;
    if (this.timer !== null) clearTimeout(this.timer);
    this.timer = null;
    this.controller?.abort();
    this.controller = null;
  }

  private schedule() {
    if (this.readers === 0) return;
    this.timer = setTimeout(() => void this.poll(), POLL_MS);
  }

  private async poll() {
    this.timer = null;
    const generation = this.generation;
    if (typeof document !== 'undefined' && document.hidden && this.through !== null) {
      this.schedule();
      return;
    }
    if (this.through === null) this.status.value = 'loading';
    const controller = new AbortController();
    this.controller = controller;
    const deadline = setTimeout(() => controller.abort(), TIMEOUT_MS);
    try {
      const url = this.through === null ? `/api/decodes?limit=${MAX_KEPT}` : `/api/decodes?since=${this.through}&limit=${MAX_KEPT}`;
      const answer = parseDecodes(await this.load(url, controller.signal));
      if (generation !== this.generation) return;
      if (!answer) throw new Error('malformed');
      if (this.through !== null && (answer.epoch !== this.epoch || answer.through < this.through)) {
        // The receiver restarted: its numbers began again at 1 and what this
        // page holds is from before. Start over with its newest.
        this.through = null;
        this.epoch = null;
        this.list.value = [];
        clearTimeout(deadline);
        if (this.controller === controller) this.controller = null;
        void this.poll();
        return;
      }
      this.list.value = mergeDecodes(this.list.value, answer.decodes);
      this.through = answer.through;
      this.epoch = answer.epoch;
      this.status.value = 'live';
    } catch {
      if (generation !== this.generation) return;
      this.status.value = 'error';
    } finally {
      clearTimeout(deadline);
      if (this.controller === controller) this.controller = null;
    }
    if (generation === this.generation) this.schedule();
  }
}

async function fetchJson(url: string, signal: AbortSignal): Promise<unknown> {
  const response = await fetch(url, { credentials: 'same-origin', signal, cache: 'no-store' });
  if (!response.ok) throw new Error(String(response.status));
  return response.json();
}

export const decodeFeed = new DecodeFeed();

/** Great-circle distance in km between two points in degrees. */
export function distanceKm(a: { lat: number; lon: number }, b: { lat: number; lon: number }): number {
  const rad = Math.PI / 180;
  const dLat = (b.lat - a.lat) * rad;
  const dLon = (b.lon - a.lon) * rad;
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(a.lat * rad) * Math.cos(b.lat * rad) * Math.sin(dLon / 2) ** 2;
  return 2 * 6371 * Math.asin(Math.min(1, Math.sqrt(h)));
}
