/**
 * Puts `text` on the clipboard; true when it got there.
 *
 * The Clipboard API exists only on HTTPS and localhost, and many receivers
 * are reached over plain HTTP, where it is simply undefined and a copy
 * button did nothing at all. There the old way still works inside a click:
 * a selected, read-only text area and the copy command.
 */
export async function copyText(text: string): Promise<boolean> {
  if (globalThis.isSecureContext && navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch {
      // Refused, as a browser may without focus; the old way below may not be.
    }
  }
  const area = document.createElement('textarea');
  area.value = text;
  area.setAttribute('readonly', '');
  // Off screen and inert, but still selectable: display:none cannot be.
  area.style.position = 'fixed';
  area.style.top = '0';
  area.style.left = '-9999px';
  area.style.opacity = '0';
  document.body.appendChild(area);
  area.select();
  let copied = false;
  try {
    copied = document.execCommand('copy');
  } catch {
    copied = false;
  }
  area.remove();
  return copied;
}
