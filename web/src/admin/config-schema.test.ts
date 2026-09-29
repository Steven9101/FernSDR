/**
 * The schema is written by hand and the server reads its configuration ad hoc,
 * so nothing enforces that the two agree. This does: it greps the server for
 * every key it actually reads and fails if the editor does not know it.
 *
 * A key the editor does not know is one it will not complete and will draw as
 * a mistake, which is worse than no highlighting at all.
 */
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { SECTIONS } from './config-schema';

function sources(dir: string): string[] {
  return readdirSync(dir).flatMap((entry) => {
    const path = join(dir, entry);
    if (statSync(path).isDirectory()) return sources(path);
    return /\.(cpp|h)$/.test(entry) ? [path] : [];
  });
}

describe('the config schema', () => {
  const known = new Set(SECTIONS.flatMap((s) => s.settings.map((v) => v.key)));

  it('knows every key the server reads', () => {
    const read = new Set<string>();
    for (const file of sources('../server/src')) {
      const text = readFileSync(file, 'utf8');
      for (const match of text.matchAll(/\.get(?:_int|_double|_bool)?\("([a-z_0-9]+)"/g)) {
        read.add(match[1]);
      }
    }
    // These are read from JSON bodies and theme files rather than from the
    // configuration, and share the accessor's name by coincidence.
    const notConfig = new Set(['frequency', 'high', 'low', 'enabled_from_json']);
    const missing = [...read].filter((key) => !known.has(key) && !notConfig.has(key));
    expect(missing).toEqual([]);
  });

  it('knows every key the shipped example uses', () => {
    const text = readFileSync('../server/fernsdr.example.conf', 'utf8');
    const used = new Set<string>();
    for (const line of text.split('\n')) {
      const match = /^\s*([a-z_0-9]+)\s*=/.exec(line);
      if (match) used.add(match[1]);
    }
    expect([...used].filter((key) => !known.has(key))).toEqual([]);
  });
});
