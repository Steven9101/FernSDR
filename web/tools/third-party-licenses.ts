/// <reference types="node" />
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { isAbsolute, join } from 'node:path';
import type { Plugin } from 'vite';

/**
 * Writes the licence of every package the build puts into a page, next to
 * the page, as `file`.
 *
 * Most of what a page ships is bundled, minified library code, and the MIT,
 * ISC and OFL licences those libraries use ask for their notice to travel
 * with copies. The packages are found from the modules that actually ended
 * up in the output, so a library the page stops using drops out of the
 * list by itself, and the Inter font is found through its stylesheet.
 */
export function thirdPartyLicenses(file: string, heading: string): Plugin {
  let root = process.cwd();
  return {
    name: 'fernsdr-third-party-licenses',
    apply: 'build',
    configResolved(config) {
      root = config.root;
    },
    generateBundle(_options, bundle) {
      const packages = new Map<string, string>();
      for (const output of Object.values(bundle)) {
        // Code through the modules it was built from; fonts and other files
        // through where they were copied from, since a stylesheet's @import
        // is not a module.
        const sources = output.type === 'chunk' ? output.moduleIds : output.originalFileNames;
        for (const id of sources) {
          const found = packageOf(isAbsolute(id) ? id : join(root, id));
          if (found) packages.set(found.name, found.directory);
        }
      }
      const sections = [...packages.entries()]
        .sort(([a], [b]) => a.localeCompare(b))
        .map(([name, directory]) => section(name, directory));
      this.emitFile({ type: 'asset', fileName: file, source: `${heading}\n\n${sections.join('\n\n')}\n` });
    },
  };
}

/** The package a module belongs to, from the last node_modules in its path. */
function packageOf(id: string): { name: string; directory: string } | null {
  const path = id.replace(/\?.*$/, '').replace(/^\0/, '');
  const at = path.lastIndexOf('/node_modules/');
  if (at < 0) return null;
  const parts = path.slice(at + '/node_modules/'.length).split('/');
  const name = parts[0].startsWith('@') ? `${parts[0]}/${parts[1]}` : parts[0];
  return { name, directory: join(path.slice(0, at), 'node_modules', name) };
}

function section(name: string, directory: string): string {
  const manifest = JSON.parse(readFileSync(join(directory, 'package.json'), 'utf8')) as {
    version?: string;
    license?: string;
  };
  const licence = readdirSync(directory).find((entry) => /^(licen[cs]e|copying)(\.(md|txt))?$/i.test(entry));
  const text = licence && existsSync(join(directory, licence))
    ? readFileSync(join(directory, licence), 'utf8').trim()
    : `Licensed under ${manifest.license ?? 'terms the package does not state'}; the package ships no licence text.`;
  return `== ${name} ${manifest.version ?? ''} (${manifest.license ?? 'see below'}) ==\n\n${text}`;
}
