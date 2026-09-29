/*
Reading and rewriting one section of config.ini as text, for pages that edit a section with a form
rather than in the editor. The rest of the file, comments included, is left exactly as it was: the
operator's notes are theirs. Lines are read the way the receiver reads them (util/config.cpp):
'#' and ';' start a comment outside quotes, and a line starting with '[' is a header.
*/

function content(line: string): string {
  let quoted = false;
  for (let i = 0; i < line.length; i++) {
    if (line[i] === '"') quoted = !quoted;
    if (!quoted && (line[i] === '#' || line[i] === ';')) return line.slice(0, i).trim();
  }
  return line.trim();
}

function headerName(line: string): string | null {
  const text = content(line);
  return text.startsWith('[') && text.endsWith(']') ? text.slice(1, -1).trim() : null;
}

/** Where each section called `name` starts and where its last setting is, by line. */
function spans(lines: string[], name: string): { start: number; end: number }[] {
  const found: { start: number; end: number }[] = [];
  for (let i = 0; i < lines.length; i++) {
    if (headerName(lines[i]) !== name) continue;
    let end = i;
    let j = i + 1;
    for (; j < lines.length && headerName(lines[j]) === null; j++) {
      if (content(lines[j])) end = j;
    }
    // Comments and blank lines after the last setting stay where they are:
    // they usually describe the section that follows.
    found.push({ start: i, end });
    i = j - 1;
  }
  return found;
}

/** The names of every section starting with `prefix:`, in file order. */
export function sectionNames(text: string, prefix: string): string[] {
  return text.split('\n').map(headerName).filter((name): name is string => name !== null && name.startsWith(`${prefix}:`));
}

/** The settings of section `name`, the last value winning as it does in the receiver; null if absent. */
export function readSection(text: string, name: string): Map<string, string> | null {
  const lines = text.split('\n');
  const found = spans(lines, name);
  if (found.length === 0) return null;
  const values = new Map<string, string>();
  for (const { start, end } of found) {
    for (let i = start + 1; i <= end; i++) {
      const line = content(lines[i]);
      const equals = line.indexOf('=');
      if (equals <= 0) continue;
      let value = line.slice(equals + 1).trim();
      if (value.length >= 2 && value.startsWith('"') && value.endsWith('"')) value = value.slice(1, -1);
      values.set(line.slice(0, equals).trim(), value);
    }
  }
  return values;
}

/**
 * `text` with section `name` holding exactly `values`, in their order: rewritten where it stands,
 * added at the end when it is new, or taken out when `values` is null. A second copy of the
 * section goes too: the receiver refuses two decoder sections of one name and reads only the first
 * of most others, so a copy is at best dead text.
 */
export function writeSection(text: string, name: string, values: Map<string, string> | null): string {
  const lines = text.split('\n');
  const found = spans(lines, name);
  const body = values
    ? [`[${name}]`, ...[...values].map(([key, value]) => `${key} = ${/[#;]/.test(value) ? `"${value}"` : value}`)]
    : [];
  if (found.length === 0) {
    if (!values) return text;
    const trimmed = text.replace(/\n*$/, '');
    return `${trimmed}${trimmed ? '\n\n' : ''}${body.join('\n')}\n`;
  }
  let result = lines;
  for (let k = found.length - 1; k >= 0; k--) {
    const { start, end } = found[k];
    result = [...result.slice(0, start), ...(k === 0 ? body : []), ...result.slice(end + 1)];
  }
  // A removed section leaves no run of blank lines behind it.
  return result.join('\n').replace(/\n{3,}/g, '\n\n');
}

/**
 * `text` with `key` in the first section called `name` set to `value`: the line that holds it
 * rewritten, or a new line after the section's last setting. Everything else, comments inside the
 * section included, stays. `text` unchanged when there is no such section.
 */
export function setSectionValue(text: string, name: string, key: string, value: string): string {
  const lines = text.split('\n');
  const found = spans(lines, name);
  if (found.length === 0) return text;
  const { start, end } = found[0];
  const line = `${key} = ${/[#;]/.test(value) ? `"${value}"` : value}`;
  for (let i = end; i > start; i--) {
    const current = content(lines[i]);
    const equals = current.indexOf('=');
    if (equals > 0 && current.slice(0, equals).trim() === key) {
      lines[i] = line;
      return lines.join('\n');
    }
  }
  lines.splice(end + 1, 0, line);
  return lines.join('\n');
}
