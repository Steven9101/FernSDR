import { cwOffsetHz } from './cw';

export function presetPassband(mode: string, pitch: number, low: number, high: number) {
  if (mode === 'cw' || mode === 'cwl') {
    const offset = cwOffsetHz(mode, pitch);
    return { low: offset + low, high: offset + high };
  }
  return mode === 'lsb' ? { low: -high, high: -low } : { low, high };
}

// These are also the server defaults. Apply them with the mode change so a
// drag before the reply cannot send the previous mode's filter back to it.
export function defaultPassband(mode: string, pitch: number) {
  if (mode === 'cw' || mode === 'cwl') return presetPassband(mode, pitch, -250, 250);
  if (mode === 'am' || mode === 'sam') return { low: -4500, high: 4500 };
  if (mode === 'nfm') return { low: -6000, high: 6000 };
  if (mode === 'wfm') return { low: -100000, high: 100000 };
  if (mode === 'dsb') return { low: -2700, high: 2700 };
  return presetPassband(mode, pitch, 300, 2700);
}

export function constrainPassband(low: number, high: number, minimum: number, maximum: number,
  width: number, edge?: 'low' | 'high') {
  if (![low, high, minimum, maximum, width].every(Number.isFinite) || maximum - minimum < 50) return null;
  if (edge === 'low') low = Math.max(minimum, high - width, Math.min(low, high - 50));
  else if (edge === 'high') high = Math.min(maximum, low + width, Math.max(high, low + 50));
  else {
    low = Math.max(minimum, Math.min(low, maximum - 50));
    high = Math.max(low + 50, Math.min(high, maximum));
    if (high - low > width) {
      const center = (low + high) / 2;
      low = center - width / 2;
      high = center + width / 2;
    }
  }
  return { low, high };
}

/**
 * The passband as a radio operator reads it: audio edges for SSB (300 to
 * 2700 for LSB as for USB), edges around the note for CW, and the carrier
 * offsets for the modes that use both sides. The inverse of presetPassband.
 */
export function displayPassband(mode: string, pitch: number, low: number, high: number) {
  if (mode === 'cw' || mode === 'cwl') {
    const offset = cwOffsetHz(mode, pitch);
    return { low: low - offset, high: high - offset };
  }
  return mode === 'lsb' ? { low: -high, high: -low } : { low, high };
}

/**
 * The passband moved so its centre sits `shift` Hz from where the mode's
 * default puts it, keeping its width: IF shift. Stateless, so the shift a
 * slider shows is always read back from the passband itself.
 */
export function shiftedPassband(mode: string, pitch: number, low: number, high: number, shift: number) {
  const shown = displayPassband(mode, pitch, low, high);
  const home = displayPassband(mode, pitch, defaultPassband(mode, pitch).low, defaultPassband(mode, pitch).high);
  const centre = (home.low + home.high) / 2 + shift;
  const half = (shown.high - shown.low) / 2;
  return presetPassband(mode, pitch, Math.round(centre - half), Math.round(centre + half));
}

/** How far the passband's centre sits from the mode's default, in the operator's terms. */
export function passbandShift(mode: string, pitch: number, low: number, high: number): number {
  const shown = displayPassband(mode, pitch, low, high);
  const home = displayPassband(mode, pitch, defaultPassband(mode, pitch).low, defaultPassband(mode, pitch).high);
  return Math.round((shown.low + shown.high) / 2 - (home.low + home.high) / 2);
}
