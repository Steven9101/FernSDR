/*
The receiver's log lines, taken apart so each part can be read on its own. The server writes
`04:11:44.463 INF [app] user 19 disconnected`; drawn as one run of text, the times, the
subsystems and the messages all weigh the same and nothing can be scanned. A line that does not
parse is kept exactly as it came: a log viewer that hides what it does not understand is worse
than one that does not try.
*/

export type LogLevel = 'debug' | 'info' | 'warn' | 'error' | 'plain';

export interface LogEntry {
  /** The line as the server wrote it. */
  raw: string;
  /** hh:mm:ss, without the milliseconds nobody reads; empty when the line did not parse. */
  time: string;
  level: LogLevel;
  /** The subsystem in the brackets. */
  where: string;
  text: string;
  /** How many identical lines in a row this stands for. */
  count: number;
  /** The first of those lines' time, when there were several. */
  since: string;
}

const LEVELS: Record<string, LogLevel> = { DBG: 'debug', INF: 'info', WRN: 'warn', ERR: 'error' };

export function parseLogLine(raw: string): LogEntry {
  const match = /^(\d{2}:\d{2}:\d{2})\.\d{3}\s+(TRC|DBG|INF|WRN|ERR)\s+\[([^\]]*)\]\s?([\s\S]*)$/.exec(raw);
  if (!match) return { raw, time: '', level: 'plain', where: '', text: raw, count: 1, since: '' };
  return {
    raw,
    time: match[1],
    level: LEVELS[match[2]] ?? 'debug',
    where: match[3],
    text: match[4],
    count: 1,
    since: match[1],
  };
}

/**
 * Runs of the same message, one after another, as one entry with a count: a source failing every
 * second says so once, with how often, instead of pushing everything else off the screen. Only
 * neighbours merge, so the order of what happened is kept.
 */
export function collapseRepeats(entries: LogEntry[]): LogEntry[] {
  const out: LogEntry[] = [];
  for (const entry of entries) {
    const last = out[out.length - 1];
    if (last && last.level === entry.level && last.where === entry.where && last.text === entry.text) {
      out[out.length - 1] = { ...entry, count: last.count + entry.count, since: last.since };
    } else {
      out.push({ ...entry });
    }
  }
  return out;
}

/**
 * The distinct problems among the lines, latest first: each warning or error once, with how many
 * times it came and when it came last.
 */
export function recentProblems(entries: LogEntry[], limit: number): LogEntry[] {
  const latest = new Map<string, LogEntry>();
  for (const entry of entries) {
    if (entry.level !== 'warn' && entry.level !== 'error') continue;
    const key = `${entry.level} ${entry.where} ${entry.text}`;
    const before = latest.get(key);
    latest.delete(key);
    latest.set(key, { ...entry, count: (before?.count ?? 0) + entry.count, since: before?.since ?? entry.since });
  }
  return [...latest.values()].reverse().slice(0, limit);
}
