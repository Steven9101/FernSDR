import type { MachineState } from '../api';

/** The cores FernSDR can really use: a quota below the CPUs it may run on is the tighter bound. */
export function usableCores(machine: MachineState): number {
  const limit = machine.cpu_limit ?? 0;
  return limit > 0 ? Math.min(limit, machine.cores) : machine.cores;
}

/**
 * FernSDR's CPU as a share of what it can use, 0 to 100. The receiver reports it per core, as top
 * does, which reads as alarming 180% on a four-core machine that is mostly idle.
 */
export function processorShare(machine: MachineState): number {
  const cores = usableCores(machine);
  if (cores <= 0) return 0;
  return Math.min(100, Math.max(0, machine.process_cpu / cores));
}

/** "4 cores", or "1.5 cores" under a quota; never "4.00". */
export function coresLabel(machine: MachineState): string {
  const cores = usableCores(machine);
  const shown = Number.isInteger(cores) ? String(cores) : cores.toFixed(1).replace(/\.0$/, '');
  return `${shown} ${cores === 1 ? 'core' : 'cores'}`;
}

/**
 * Worth the operator's attention: the receiver near its CPU ceiling drops audio, and a machine
 * with little memory left starts swapping or has FernSDR killed.
 */
export function processorTight(machine: MachineState): boolean {
  return processorShare(machine) >= 85;
}

export function memoryTight(machine: MachineState): boolean {
  return machine.memory_total > 0 && machine.memory_available / machine.memory_total < 0.1;
}
