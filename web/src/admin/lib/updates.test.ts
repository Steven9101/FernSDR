import { describe, expect, it } from 'vitest';
import type { UpdateStatus } from '../api';
import { machineName, stepLabel, updating } from './updates';

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
