/**
 * Where a CW signal sits relative to the frequency the receiver is tuned to.
 *
 * CW is heard as a beat note, so the signal cannot sit on the tuned frequency:
 * a zero-beat is silence. The server places the CW filter at the pitch: the
 * passband is {pitch-250, pitch+250} for CW and the mirror of that for CW-L,
 * so a signal is audible when it is exactly `pitch` away from the carrier.
 *
 * Every operator-facing frequency is therefore the *signal*, and the carrier
 * the receiver is actually tuned to is offset behind it. Showing the carrier
 * instead would mean clicking a signal tuned 700 Hz off it, and logging a
 * contact on a frequency nobody else would recognise.
 */
export type CwOffsetMode = string;

export function cwOffsetHz(mode: CwOffsetMode, cwPitchHz: number): number {
  const pitch = Number.isFinite(cwPitchHz) ? cwPitchHz : 0;
  const lower = mode === 'cwl' || mode === 'cw-l' || mode === 'cw_l';
  if (lower) return -pitch;
  return mode === 'cw' ? pitch : 0;
}

/** The frequency to tune so that `signalHz` lands in the passband. */
export function carrierForSignal(signalHz: number, mode: CwOffsetMode, cwPitchHz: number): number {
  return signalHz - cwOffsetHz(mode, cwPitchHz);
}

/** The frequency a listener should see for a receiver tuned to `carrierHz`. */
export function signalForCarrier(carrierHz: number, mode: CwOffsetMode, cwPitchHz: number): number {
  return carrierHz + cwOffsetHz(mode, cwPitchHz);
}

/**
 * A CW filter's carrier-relative edges, kept at one pitch, moved to another.
 * The edges are where the note was; restored at a different pitch without
 * this, the signal lands the pitch difference outside the filter and is
 * silent. Without a kept pitch (entries from before one was kept), the edges
 * are used as they are.
 */
export function edgesAtPitch(mode: CwOffsetMode, low: number, high: number, keptPitchHz: number | undefined,
  pitchHz: number): { low: number; high: number } {
  if (keptPitchHz === undefined || !Number.isFinite(keptPitchHz)) return { low, high };
  const shift = cwOffsetHz(mode, pitchHz) - cwOffsetHz(mode, keptPitchHz);
  return { low: low + shift, high: high + shift };
}
