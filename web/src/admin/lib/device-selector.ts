/**
 * The `device` setting that picks one radio among several a module drives.
 *
 * Identity is the port the radio is on, not the object that described it: a
 * fresh look at the hardware describes the same radios with new objects, and
 * a radio chosen before it must still be found in the new list.
 */
export interface RadioSeen {
  port: string;
  module: string;
  serial?: string;
}

/**
 * `serial:…` where the radio has a serial number, `index:n` among the radios
 * of its module otherwise, and '' where it is the only one, which the module
 * finds by itself. Null when the chosen radio is not in `radios`, so nothing
 * is written that names a radio which is not there.
 */
export function deviceSelector(radios: readonly RadioSeen[], chosen: RadioSeen): string | null {
  const same = radios.filter((radio) => radio.module === chosen.module);
  const index = same.findIndex((radio) => radio.port === chosen.port);
  if (index < 0) return null;
  if (same.length < 2) return '';
  const serial = same[index].serial;
  return serial ? `serial:${serial}` : `index:${index}`;
}
