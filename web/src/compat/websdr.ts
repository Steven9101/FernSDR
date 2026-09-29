/**
 * Lets programs that sync a transceiver with web receivers (CATSync, and
 * the tools that work the same way) treat this page as a PA3FWM WebSDR, the
 * receiver they have supported longest.
 *
 * Those programs are a browser with the radio attached. They recognise a
 * WebSDR by the word "setfreqif" in the page's HTML, tune it by writing
 * kilohertz into the `frequency` field of the form named `freqform` and
 * calling `setfreqif` (or submitting that form), set the mode with
 * `set_mode('usb')`, and read the page back from the same field after every
 * click and wheel turn. index.html carries the form, hidden; this keeps its
 * field on the frequency the dial shows, and turns what arrives in it into
 * tuning.
 *
 * Nothing here talks to the radio or leaves the page: a script in the page
 * could tune the receiver anyway.
 */
import { controller, tuning } from '../state/store';
import { watch } from '../state/reactive.svelte';
import { signalForCarrier } from '../util/cw';
import { formatKhz, parseKhz } from './khz';

/** WebSDR's mode names, and what they mean here. */
const MODES: Record<string, string> = { usb: 'usb', lsb: 'lsb', cw: 'cw', am: 'am', fm: 'nfm', nfm: 'nfm' };

export function installWebSdrCompatibility(doc: Document = document, win: Window = window): () => void {
  const form = doc.forms.namedItem('freqform');
  const field = form?.elements.namedItem('frequency');
  if (!form || !(field instanceof HTMLInputElement)) return () => {};

  const tuneTo = (text: unknown) => {
    const hz = parseKhz(text);
    if (hz !== null) controller.goTo(hz);
  };

  const globals = win as unknown as Record<string, unknown>;
  globals.setfreqif = (text: unknown) => tuneTo(text ?? field.value);
  globals.set_mode = (mode: unknown) => {
    const name = MODES[String(mode).toLowerCase()];
    if (name && name !== tuning.value.mode) controller.setMode(name);
  };
  const onSubmit = (event: Event) => {
    event.preventDefault();
    tuneTo(field.value);
  };
  form.addEventListener('submit', onSubmit);
  // form.submit() skips the submit event and would load the page again with
  // the frequency in the address; here it tunes instead.
  (form as unknown as { submit: () => void }).submit = () => tuneTo(field.value);

  const stop = watch(() => {
    const t = tuning.value;
    field.value = formatKhz(signalForCarrier(t.freq, t.mode, t.cwPitch));
  });

  return () => {
    stop();
    form.removeEventListener('submit', onSubmit);
    delete globals.setfreqif;
    delete globals.set_mode;
  };
}
