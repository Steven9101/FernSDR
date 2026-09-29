/*
How the Updates page words the updater's progress. The updater writes a state and a sentence;
these are the short labels and the one question the page has to answer on every poll: is an
update under way, so the page should keep looking and offer nothing else.
*/
import type { UpdateStatus } from '../api';

const underWay: ReadonlySet<UpdateStatus['state']> = new Set(['checking', 'downloading', 'installing', 'trial']);

export function updating(status: UpdateStatus | undefined): boolean {
  return status !== undefined && underWay.has(status.state);
}

export function stepLabel(status: UpdateStatus): string {
  switch (status.state) {
    case 'checking':
      return `Fetching ${status.version}`;
    case 'downloading':
      return `Downloading ${status.version}`;
    case 'installing':
      return `Unpacking ${status.version}`;
    case 'trial':
      return `Trying ${status.version}`;
    case 'updated':
      return `Updated to ${status.version}`;
    case 'rolled-back':
      return `${status.version} was rolled back`;
    case 'refused':
      return `${status.version} was not installed`;
    case 'failed':
      return 'The update failed';
  }
}

/** linux-aarch64 as an operator knows it. */
export function machineName(platform: string): string {
  switch (platform) {
    case 'linux-x86_64':
      return 'Linux, 64-bit PC';
    case 'linux-aarch64':
      return 'Linux, 64-bit ARM';
    case 'linux-armhf':
      return 'Linux, 32-bit ARM';
    default:
      return platform || 'not one releases are built for';
  }
}

const DISMISSED_KEY = 'fernsdr.update.dismissed';
const LOOKED_KEY = 'fernsdr.update.looked';
const DAY_S = 24 * 60 * 60;

/** Puts a version's offer away in this browser; a newer version is offered again. */
export function dismissUpdate(version: string): void {
  try {
    localStorage.setItem(DISMISSED_KEY, version);
  } catch {
    // No storage: the offer comes back at the next sign-in.
  }
}

export function updateDismissed(version: string): boolean {
  try {
    return localStorage.getItem(DISMISSED_KEY) === version;
  } catch {
    return false;
  }
}

/**
 * Whether the panel should ask the receiver to look for a newer release:
 * when it has not looked for a day, and this browser has not asked it to in
 * the last six hours, which keeps a receiver that restarts often from asking
 * GitHub at every sign-in.
 */
export function lookDue(checkedS: number | undefined, nowS: number, askedS: number): boolean {
  return (!checkedS || nowS - checkedS > DAY_S) && nowS - askedS > DAY_S / 4;
}

export function lastAsked(): number {
  try {
    return Number(localStorage.getItem(LOOKED_KEY)) || 0;
  } catch {
    return 0;
  }
}

export function noteAsked(nowS: number): void {
  try {
    localStorage.setItem(LOOKED_KEY, String(nowS));
  } catch {
    // Nothing to keep it in.
  }
}

/** "2026-10-01" as "1 October 2026"; anything else as it is. */
export function releaseDate(date: string): string {
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(date);
  if (!match) return date;
  const months = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
  const month = months[Number(match[2]) - 1];
  return month ? `${Number(match[3])} ${month} ${match[1]}` : date;
}
