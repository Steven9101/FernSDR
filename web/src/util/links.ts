/**
 * An address a link on the page may open: a web page, or a path on this
 * receiver. The receiver refuses anything else in the theme and the station
 * settings; this holds the page to the same where it is served without the
 * receiver's Content-Security-Policy, which is what stops a javascript:
 * address today.
 */
export function webLink(url: string | undefined | null): string | undefined {
  if (!url) return undefined;
  return /^(https?:\/\/|\/(?!\/))/i.test(url) ? url : undefined;
}
