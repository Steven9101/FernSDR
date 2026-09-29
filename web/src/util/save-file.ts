/**
 * Hands the listener a file made in the page. The object URL is let go a few
 * seconds later rather than at once: some browsers start reading it only
 * after the click has returned, and a recording can be tens of megabytes.
 */
export function saveFile(contents: Blob | string, name: string, type = 'text/plain'): void {
  const blob = typeof contents === 'string' ? new Blob([contents], { type }) : contents;
  const link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = name;
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 5000);
}
