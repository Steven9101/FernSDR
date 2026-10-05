import { describe, expect, it } from 'vitest';
import type { MachineState } from '../api';
import { coresLabel, memoryTight, processorShare, processorTight, usableCores } from './machine';

const machine = (over: Partial<MachineState> = {}): MachineState => ({
  process_cpu: 0,
  system_cpu: 0,
  cores: 4,
  process_memory: 50e6,
  memory_total: 4e9,
  memory_available: 2e9,
  ...over,
});

describe('machine figures', () => {
  it('gives the CPU as a share of the cores FernSDR may use', () => {
    expect(processorShare(machine({ process_cpu: 180 }))).toBe(45);
    expect(processorShare(machine({ process_cpu: 50, cores: 1 }))).toBe(50);
  });

  it('takes a container quota below the core count as the ceiling', () => {
    const limited = machine({ process_cpu: 120, cpu_limit: 1.5 });
    expect(usableCores(limited)).toBe(1.5);
    expect(processorShare(limited)).toBe(80);
    expect(coresLabel(limited)).toBe('1.5 cores');
    // A quota above the CPUs it may run on changes nothing.
    expect(usableCores(machine({ cpu_limit: 8 }))).toBe(4);
  });

  it('never shows more than the whole or less than nothing', () => {
    expect(processorShare(machine({ process_cpu: 900 }))).toBe(100);
    expect(processorShare(machine({ cores: 0 }))).toBe(0);
  });

  it('names cores in words a person writes', () => {
    expect(coresLabel(machine())).toBe('4 cores');
    expect(coresLabel(machine({ cores: 1 }))).toBe('1 core');
    expect(coresLabel(machine({ cpu_limit: 2 }))).toBe('2 cores');
  });

  it('flags a receiver near its ceiling or a machine short of memory', () => {
    expect(processorTight(machine({ process_cpu: 340 }))).toBe(true);
    expect(processorTight(machine({ process_cpu: 100 }))).toBe(false);
    expect(memoryTight(machine({ memory_available: 0.3e9 }))).toBe(true);
    expect(memoryTight(machine())).toBe(false);
    expect(memoryTight(machine({ memory_total: 0, memory_available: 0 }))).toBe(false);
  });
});
