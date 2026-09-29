import { readFileSync } from 'node:fs';
import { describe, expect, it } from 'vitest';

// The release number is written in two places, the server's version.h and the
// client's package.json; this keeps them from drifting apart.
describe('the release number', () => {
  it('is the same for the server and the client', () => {
    const header = readFileSync(new URL('../../../server/src/version.h', import.meta.url), 'utf8');
    const server = header.match(/kVersion = "([^"]+)"/)?.[1];
    const client = JSON.parse(readFileSync(new URL('../../package.json', import.meta.url), 'utf8')).version;
    expect(server).toMatch(/^\d+\.\d+\.\d+$/);
    expect(client).toBe(server);
  });
});
