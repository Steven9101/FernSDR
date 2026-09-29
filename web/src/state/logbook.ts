/**
 * The listener's log: what they heard, when and where, the way a short-wave
 * listener keeps one. Kept in this browser only and taken elsewhere as ADIF,
 * which every logging program reads, or CSV for a spreadsheet.
 */
import { box } from './reactive.svelte';

export interface LogEntry {
  id: string;
  /** When it was heard, ms since 1970, UTC. */
  time: number;
  /** The signal's frequency in Hz, as the dial shows it. */
  freq: number;
  mode: string;
  /** Callsign or station name. */
  station: string;
  /** The listener's own report: RST, SINPO, or whatever they use. */
  report: string;
  note: string;
}

const STORAGE_KEY = 'fernsdr.log.v1';
/**
 * Years of evenings of listening, and at about 150 bytes an entry well inside
 * the few megabytes a browser gives a page.
 */
export const MAX_LOG_ENTRIES = 5000;
const MODES = new Set(['usb', 'lsb', 'cw', 'cwl', 'am', 'sam', 'nfm', 'dsb', 'wfm']);
const LIMITS = { station: 40, report: 12, note: 200 } as const;

export const logbook = box<LogEntry[]>([]);

function text(value: unknown, limit: number): string {
  return String(value ?? '')
    .replace(/[\u0000-\u001f\u007f]/g, ' ')
    .trim()
    .slice(0, limit);
}

export function validLogEntry(value: unknown): LogEntry | null {
  if (!value || typeof value !== 'object') return null;
  const v = value as Record<string, unknown>;
  const time = Number(v.time);
  const freq = Number(v.freq);
  const mode = String(v.mode ?? '').toLowerCase();
  if (!Number.isFinite(time) || time <= 0 || !Number.isFinite(freq) || freq <= 0 || freq > 1e11 || !MODES.has(mode)) return null;
  return {
    id: typeof v.id === 'string' && /^[a-z0-9-]{1,40}$/i.test(v.id) ? v.id : newId(),
    time: Math.round(time),
    freq: Math.round(freq),
    mode,
    station: text(v.station, LIMITS.station),
    report: text(v.report, LIMITS.report),
    note: text(v.note, LIMITS.note),
  };
}

function newId(): string {
  return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
}

function save(list: LogEntry[]): void {
  logbook.value = list;
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(list));
  } catch {
    // Private browsing or storage off: the log lasts as long as the page.
  }
}

export function loadLogbook(): void {
  try {
    const parsed = JSON.parse(localStorage.getItem(STORAGE_KEY) ?? '[]');
    if (Array.isArray(parsed)) logbook.value = parsed.map(validLogEntry).filter((e): e is LogEntry => e !== null).slice(0, MAX_LOG_ENTRIES);
  } catch {
    logbook.value = [];
  }
}

/** Adds an entry at the top; null when it is not valid or the log is full. */
export function addLogEntry(entry: Omit<LogEntry, 'id'>): LogEntry | null {
  const valid = validLogEntry({ ...entry, id: newId() });
  if (!valid || logbook.value.length >= MAX_LOG_ENTRIES) return null;
  save([valid, ...logbook.value]);
  return valid;
}

export function removeLogEntry(id: string): void {
  save(logbook.value.filter((e) => e.id !== id));
}

// --- ADIF ----------------------------------------------------------------

/** ADIF 3.1 bands in MHz; a frequency outside all of them is logged without one. */
const ADIF_BANDS: [string, number, number][] = [
  ['2190m', 0.1357, 0.1378], ['630m', 0.472, 0.479], ['560m', 0.501, 0.504],
  ['160m', 1.8, 2.0], ['80m', 3.5, 4.0], ['60m', 5.06, 5.45], ['40m', 7.0, 7.3],
  ['30m', 10.1, 10.15], ['20m', 14.0, 14.35], ['17m', 18.068, 18.168], ['15m', 21.0, 21.45],
  ['12m', 24.89, 24.99], ['10m', 28.0, 29.7], ['8m', 40, 45], ['6m', 50, 54], ['5m', 54.000001, 69.9],
  ['4m', 70, 71], ['2m', 144, 148], ['1.25m', 222, 225], ['70cm', 420, 450], ['33cm', 902, 928],
  ['23cm', 1240, 1300], ['13cm', 2300, 2450],
];

export function adifBand(hz: number): string | null {
  const mhz = hz / 1e6;
  return ADIF_BANDS.find(([, low, high]) => mhz >= low && mhz <= high)?.[0] ?? null;
}

/** ADIF modes and submodes. DSB has no ADIF mode, so it goes without one. */
const ADIF_MODES: Record<string, [string, string?]> = {
  usb: ['SSB', 'USB'], lsb: ['SSB', 'LSB'], cw: ['CW'], cwl: ['CW'], am: ['AM'], sam: ['AM'], nfm: ['FM'],
};

/** Letters that would turn into '?' rather than lose an accent. */
const SPELLED: Record<string, string> = {
  ß: 'ss', æ: 'ae', Æ: 'AE', ø: 'o', Ø: 'O', œ: 'oe', Œ: 'OE', ł: 'l', Ł: 'L', đ: 'd', Đ: 'D', þ: 'th', Þ: 'TH',
};

/**
 * ADIF fields are ASCII and their lengths count characters, so accents are
 * taken off and anything left outside printable ASCII becomes '?'.
 */
function ascii(value: string): string {
  return value
    .replace(/[ßæÆøØœŒłŁđĐþÞ]/g, (letter) => SPELLED[letter])
    .normalize('NFKD')
    .replace(/[̀-ͯ]/g, '')
    .replace(/[^\x20-\x7e]/g, '?');
}

/**
 * The station as an ADIF callsign when it looks like one (letters and digits
 * with at least one of each, portable suffixes allowed); a broadcaster's name
 * such as "Radio Romania" is not one and goes into the comment instead.
 */
function callsign(station: string): string | null {
  const call = station.trim().toUpperCase();
  return /^(?=.*\d)(?=.*[A-Z])[A-Z0-9]+(\/[A-Z0-9]+)*$/.test(call) && call.length <= 20 ? call : null;
}

function field(name: string, value: string): string {
  return value ? `<${name}:${value.length}>${value} ` : '';
}

function pad(value: number, width = 2): string {
  return String(value).padStart(width, '0');
}

export function toAdif(entries: LogEntry[], now = Date.now()): string {
  const created = new Date(now);
  const stamp = `${created.getUTCFullYear()}${pad(created.getUTCMonth() + 1)}${pad(created.getUTCDate())} ${pad(created.getUTCHours())}${pad(created.getUTCMinutes())}${pad(created.getUTCSeconds())}`;
  const lines = [
    'Listening log exported from a FernSDR receiver.',
    `${field('ADIF_VER', '3.1.4')}${field('CREATED_TIMESTAMP', stamp)}${field('PROGRAMID', 'FernSDR')}<EOH>`,
  ];
  for (const entry of entries) {
    const heard = new Date(entry.time);
    const [mode, submode] = ADIF_MODES[entry.mode] ?? [];
    const station = ascii(entry.station);
    const call = callsign(station);
    const comment = call || !station ? entry.note : entry.note ? `${entry.station}; ${entry.note}` : entry.station;
    const record =
      field('CALL', call ?? '') +
      field('QSO_DATE', `${heard.getUTCFullYear()}${pad(heard.getUTCMonth() + 1)}${pad(heard.getUTCDate())}`) +
      field('TIME_ON', `${pad(heard.getUTCHours())}${pad(heard.getUTCMinutes())}${pad(heard.getUTCSeconds())}`) +
      field('FREQ', (entry.freq / 1e6).toFixed(6)) +
      field('BAND', adifBand(entry.freq) ?? '') +
      field('MODE', mode ?? '') +
      field('SUBMODE', submode ?? '') +
      field('RST_RCVD', ascii(entry.report)) +
      field('COMMENT', ascii(comment)) +
      // Heard, not worked: a logging program must not count it as a contact.
      field('SWL', 'Y');
    lines.push(`${record}<EOR>`);
  }
  return `${lines.join('\n')}\n`;
}

// --- CSV -------------------------------------------------------------------

/**
 * Quoted always; and a cell a spreadsheet would read as a formula gets a
 * leading apostrophe, since the note is free text the listener may have
 * pasted from anywhere.
 */
function cell(value: string): string {
  const safe = /^[=+\-@\t\r]/.test(value) ? `'${value}` : value;
  return `"${safe.replace(/"/g, '""')}"`;
}

export function toCsv(entries: LogEntry[]): string {
  const rows = [['UTC', 'Frequency (kHz)', 'Mode', 'Station', 'Report', 'Note'].map(cell).join(',')];
  for (const entry of entries) {
    rows.push(
      [
        new Date(entry.time).toISOString().replace('T', ' ').slice(0, 19),
        (entry.freq / 1e3).toFixed(3),
        entry.mode.toUpperCase(),
        entry.station,
        entry.report,
        entry.note,
      ].map(cell).join(','),
    );
  }
  return `${rows.join('\r\n')}\r\n`;
}
