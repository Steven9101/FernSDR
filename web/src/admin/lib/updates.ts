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
