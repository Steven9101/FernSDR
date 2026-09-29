/*
One line of the configuration file, split into what it is made of, for painting.

Deliberately not a general INI parser: the server's parser is the authority on what the file
means, and this one only has to agree with it about what the pieces look like. Where they could
disagree, a '#' inside a quoted value, it follows the same rule the server does.
*/

export type TokenKind =
  | 'space'
  | 'comment'
  | 'section'
  | 'key'
  | 'equals'
  | 'value'
  | 'number'
  | 'boolean'
  | 'url'
  | 'secret'
  | 'unknown';

export interface Token {
  text: string;
  kind: TokenKind;
}

export function tokenise(line: string): Token[] {
  const tokens: Token[] = [];
  let rest = line;

  const leading = /^\s+/.exec(rest);
  if (leading) {
    tokens.push({ text: leading[0], kind: 'space' });
    rest = rest.slice(leading[0].length);
  }

  if (rest.startsWith('#') || rest.startsWith(';')) {
    tokens.push({ text: rest, kind: 'comment' });
    return tokens;
  }

  if (rest.startsWith('[')) {
    const end = rest.indexOf(']');
    const header = end === -1 ? rest : rest.slice(0, end + 1);
    tokens.push({ text: header, kind: 'section' });
    if (end !== -1 && end + 1 < rest.length) tokens.push({ text: rest.slice(end + 1), kind: 'space' });
    return tokens;
  }

  const equals = rest.indexOf('=');
  if (equals === -1) {
    if (rest) tokens.push({ text: rest, kind: rest.trim() ? 'unknown' : 'space' });
    return tokens;
  }

  const key = rest.slice(0, equals);
  const trimmed = key.trimEnd();
  if (trimmed) tokens.push({ text: trimmed, kind: 'key' });
  if (key.length !== trimmed.length) tokens.push({ text: key.slice(trimmed.length), kind: 'space' });
  tokens.push({ text: '=', kind: 'equals' });
  // Operators share screenshots of this page; a password hash is not something to paint.
  const secret = /password|secret|token/i.test(trimmed);

  let value = rest.slice(equals + 1);
  // A comment may follow a value, and a '#' inside quotes is not one.
  let quoted = false;
  let cut = -1;
  for (let i = 0; i < value.length; i++) {
    if (value[i] === '"') quoted = !quoted;
    if (!quoted && (value[i] === '#' || value[i] === ';')) {
      cut = i;
      break;
    }
  }
  let trailing = '';
  if (cut !== -1) {
    trailing = value.slice(cut);
    value = value.slice(0, cut);
  }

  const before = /^\s*/.exec(value)?.[0] ?? '';
  const after = /\s*$/.exec(value.slice(before.length))?.[0] ?? '';
  const core = value.slice(before.length, value.length - after.length);
  if (before) tokens.push({ text: before, kind: 'space' });
  if (core) {
    // Values are coloured by shape, which is what makes a typo visible: a frequency that lost its
    // suffix stops looking like a frequency. The whole value has the shape, or it is text: a
    // name with a number in it, such as "RTL-SDR 20 m", is a name.
    const parts = core.split(/(\s+|,)/).filter(Boolean);
    const words = parts.filter((part) => !/^\s+$/.test(part) && part !== ',');
    if (secret) tokens.push({ text: core, kind: 'secret' });
    else if (words.length > 0 && words.every((word) => /^-?[\d.]+[kKmMgG]?$/.test(word))) {
      for (const part of parts) tokens.push({ text: part, kind: /^\s+$/.test(part) || part === ',' ? 'space' : 'number' });
    } else if (/^(true|false|yes|no|on|off)$/i.test(core)) tokens.push({ text: core, kind: 'boolean' });
    else if (/^[a-z]+:\/\/\S+$/i.test(core)) tokens.push({ text: core, kind: 'url' });
    else tokens.push({ text: core, kind: 'value' });
  }
  if (after) tokens.push({ text: after, kind: 'space' });
  if (trailing) tokens.push({ text: trailing, kind: 'comment' });
  return tokens;
}

/**
 * `text` with `key = value` in section `section`, keeping everything else as it was: the other
 * lines, their comments, a comment after the value being replaced, and the file's own line
 * endings. The server takes the last of a key given twice, so that is the one changed. A key the
 * section lacks is added after its last setting. Null when there is no such section.
 */
export function setKey(text: string, section: string, key: string, value: string): string | null {
  const ending = text.includes('\r\n') ? '\r\n' : '\n';
  const lines = text.split(/\r?\n/);
  let inside = false;
  let found = false;
  let lastSetting = -1;
  let lastMatch = -1;
  for (let index = 0; index < lines.length; index++) {
    const header = /^\s*\[([^\]]*)\]/.exec(lines[index]);
    if (header) {
      if (inside) break;
      inside = header[1].trim() === section;
      if (inside) {
        found = true;
        lastSetting = index;
      }
      continue;
    }
    if (!inside) continue;
    const setting = /^\s*([^=#;\s]+)\s*=/.exec(lines[index]);
    if (!setting) continue;
    lastSetting = index;
    if (setting[1] === key) lastMatch = index;
  }
  if (!found) return null;
  if (lastMatch === -1) {
    lines.splice(lastSetting + 1, 0, `${key} = ${value}`);
    return lines.join(ending);
  }
  const setting = /^(\s*)([^=#;\s]+)(\s*=\s*)(.*)$/.exec(lines[lastMatch])!;
  // Keep a comment that follows the value; a '#' inside quotes is not one.
  const rest = setting[4];
  let quoted = false;
  let cut = -1;
  for (let i = 0; i < rest.length; i++) {
    if (rest[i] === '"') quoted = !quoted;
    if (!quoted && (rest[i] === '#' || rest[i] === ';')) {
      cut = i;
      break;
    }
  }
  const comment = cut === -1 ? '' : ` ${rest.slice(cut).trim()}`;
  lines[lastMatch] = `${setting[1]}${key}${setting[3]}${value}${comment}`;
  return lines.join(ending);
}

/**
 * The value the server reads for `key` in `section`, or null: the last one given in the first
 * section of that name, without its comment or the quotes around it, as the server's parser takes
 * it.
 */
export function readKey(text: string, section: string, key: string): string | null {
  let inside = false;
  let value: string | null = null;
  for (const line of text.split(/\r?\n/)) {
    let code = line;
    let quoted = false;
    for (let i = 0; i < line.length; i++) {
      if (line[i] === '"') quoted = !quoted;
      if (!quoted && (line[i] === '#' || line[i] === ';')) {
        code = line.slice(0, i);
        break;
      }
    }
    code = code.trim();
    if (code.startsWith('[')) {
      if (inside) break;
      inside = code.slice(1, -1).trim() === section;
      continue;
    }
    const equals = code.indexOf('=');
    if (!inside || equals === -1 || code.slice(0, equals).trim() !== key) continue;
    value = code.slice(equals + 1).trim();
    if (value.length >= 2 && value.startsWith('"') && value.endsWith('"')) value = value.slice(1, -1);
  }
  return value;
}
