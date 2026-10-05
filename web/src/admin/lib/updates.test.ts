import { describe, expect, it } from 'vitest';
import type { AutostartView, UpdateStatus } from '../api';
import {
  autostartChecked,
  autostartDetail,
  autostartFooter,
  lookDue,
  machineName,
  releaseDate,
  stepLabel,
  updating
} from './updates';

const status = (state: UpdateStatus['state']): UpdateStatus => ({ state, version: '0.1.1', message: '', time: 1 });

describe('update progress', () => {
  it('is under way from fetching through the trial, and not before or after', () => {
    for (const state of ['checking', 'downloading', 'installing', 'trial'] as const) {
      expect(updating(status(state))).toBe(true);
    }
    for (const state of ['updated', 'rolled-back', 'refused', 'failed'] as const) {
      expect(updating(status(state))).toBe(false);
    }
    expect(updating(undefined)).toBe(false);
  });

  it('names each step with the version', () => {
    expect(stepLabel(status('downloading'))).toBe('Downloading 0.1.1');
    expect(stepLabel(status('trial'))).toBe('Trying 0.1.1');
    expect(stepLabel(status('rolled-back'))).toBe('0.1.1 was rolled back');
  });

  it('names machines the way an operator knows them', () => {
    expect(machineName('linux-aarch64')).toBe('Linux, 64-bit ARM');
    expect(machineName('')).toBe('not one releases are built for');
  });
});

describe('the update offer', () => {
  const day = 24 * 60 * 60;
  it('asks the receiver to look once a day, and not again within six hours of asking', () => {
    const now = 100 * day;
    expect(lookDue(undefined, now, 0)).toBe(true);
    expect(lookDue(now - 2 * day, now, 0)).toBe(true);
    expect(lookDue(now - 3600, now, 0)).toBe(false);
    // The receiver restarted and forgot, but this browser asked an hour ago.
    expect(lookDue(undefined, now, now - 3600)).toBe(false);
    expect(lookDue(undefined, now, now - 7 * 3600)).toBe(true);
  });

  it('writes the release date out', () => {
    expect(releaseDate('2026-10-01')).toBe('1 October 2026');
    expect(releaseDate('soon')).toBe('soon');
  });
});

describe('starting with the computer', () => {
  const view = (change: Partial<AutostartView> = {}): AutostartView => ({
    init: 'systemd',
    enabled: false,
    changeable: true,
    ...change
  });

  it('shows what was asked for until the updater takes it, then what the init says', () => {
    expect(autostartChecked(view())).toBe(false);
    expect(autostartChecked(view({ pending: 'on' }))).toBe(true);
    expect(autostartDetail(view({ pending: 'on' }))).toBe('Switching on');
    // Taken and failed: the init still says off, and so does the switch.
    const failed = view({ result: { time: 5, ok: false, message: 'update-rc.d exited with status 1' } });
    expect(autostartChecked(failed)).toBe(false);
    expect(autostartFooter(failed)).toEqual({ text: 'update-rc.d exited with status 1', warning: true });
    // Not while the next one waits.
    expect(autostartFooter({ ...failed, pending: 'on' })?.warning).toBe(false);
  });

  it('says why it cannot be switched just now before anything else', () => {
    const busy = view({ blocked: 'An update is under way.', result: { time: 5, ok: false, message: 'x' } });
    expect(autostartFooter(busy)).toEqual({ text: 'An update is under way.', warning: false });
  });

  it('gives the sentence instead of a switch where the panel cannot switch it', () => {
    const docker = view({ init: 'container', changeable: false, note: 'Docker decides.' });
    expect(autostartDetail(docker)).toBe('Docker decides.');
    expect(autostartFooter(docker)).toBeNull();
  });

  it('says what each state means', () => {
    expect(autostartDetail(view({ enabled: true }))).toBe('FernSDR starts when the computer does');
    expect(autostartDetail(view())).toBe('Start it by hand after the computer starts');
    expect(autostartFooter(view())?.text).toContain('keeps running now');
  });
});
