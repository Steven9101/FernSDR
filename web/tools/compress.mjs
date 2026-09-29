import { readdir, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync } from 'node:zlib';

async function compress(directory) {
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) await compress(path);
    else if (/\.(?:html|js|css|svg|json|txt)$/.test(entry.name)) {
      const data = await readFile(path);
      const gzip = gzipSync(data, { level: 9 });
      if (gzip.length < data.length) await writeFile(`${path}.gz`, gzip);
    }
  }
}

const dist = fileURLToPath(new URL('../dist/', import.meta.url));
await compress(dist);
// The installer also accepts files built on another machine. It must not
// pair an older client with this server: v2 was the admin signing protocol,
// v3 is pages without inline scripts, which the server's
// Content-Security-Policy would block.
await writeFile(join(dist, 'fernsdr-client-v3'), 'admin signing protocol 2, no inline scripts\n');
