/**
 * A number typed into a field, or null when there is none. A number input bound in Svelte gives
 * null when it is cleared, and Number(null) is 0, which is a value: read that way, clearing a
 * gain field and pressing Set sent a gain of 0 to a dongle everyone was listening through.
 */
export function typedNumber(raw: unknown): number | null {
  if (raw === null || raw === undefined || typeof raw === 'boolean') return null;
  if (typeof raw === 'number') return Number.isFinite(raw) ? raw : null;
  const text = String(raw).trim();
  if (text === '') return null;
  const value = Number(text);
  return Number.isFinite(value) ? value : null;
}
