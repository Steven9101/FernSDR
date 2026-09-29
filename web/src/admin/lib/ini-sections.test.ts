import { describe, expect, it } from 'vitest';
import { readSection, sectionNames, setSectionValue, writeSection } from './ini-sections';

const file = `# The receiver
[site]
name = Home ; the name

[band:40m]
source = test

# FT8 on two bands
[decoder:ft8]
module = ft8   # the module
channels = 40m:7074000
public = no

# Keep this note
[admin]
password_hash = (kept on the machine)
`;

describe('config sections', () => {
  it('reads a section the way the receiver does', () => {
    expect(sectionNames(file, 'decoder')).toEqual(['decoder:ft8']);
    expect(readSection(file, 'decoder:ft8')).toEqual(new Map([['module', 'ft8'], ['channels', '40m:7074000'], ['public', 'no']]));
    expect(readSection(file, 'decoder:wspr')).toBeNull();
    expect(readSection('[x]\na = "b # c"\n', 'x')?.get('a')).toBe('b # c');
  });

  it('rewrites a section in place and leaves every other line alone', () => {
    const next = writeSection(file, 'decoder:ft8', new Map([['module', 'ft8'], ['channels', '40m:7074000 20m:14074000'], ['public', 'yes']]));
    expect(next).toBe(file.replace('module = ft8   # the module\nchannels = 40m:7074000\npublic = no',
      'module = ft8\nchannels = 40m:7074000 20m:14074000\npublic = yes'));
  });

  it('adds a new section at the end and takes one out without leaving a gap', () => {
    const added = writeSection(file, 'decoder:wspr', new Map([['module', 'wspr']]));
    expect(added.endsWith('(kept on the machine)\n\n[decoder:wspr]\nmodule = wspr\n')).toBe(true);
    const removed = writeSection(file, 'decoder:ft8', null);
    expect(removed).not.toContain('decoder:ft8');
    expect(removed).toContain('# FT8 on two bands\n\n# Keep this note\n[admin]');
    expect(removed).not.toMatch(/\n{3}/);
    expect(writeSection('', 'decoder:a', new Map([['module', 'a']]))).toBe('[decoder:a]\nmodule = a\n');
  });

  it('merges a section written twice into one', () => {
    const twice = '[decoder:a]\nmodule = a\n[site]\nname = x\n[decoder:a]\npublic = yes\n';
    expect(readSection(twice, 'decoder:a')).toEqual(new Map([['module', 'a'], ['public', 'yes']]));
    expect(writeSection(twice, 'decoder:a', new Map([['module', 'b']]))).toBe('[decoder:a]\nmodule = b\n[site]\nname = x\n');
  });

  it('quotes a value that would otherwise read as a comment', () => {
    expect(writeSection('', 'x', new Map([['note', 'a # b']]))).toBe('[x]\nnote = "a # b"\n');
  });
});

describe('one setting', () => {
  const bands = `[band:40m]
source = module
module = rtlsdr   # the dongle on the roof
module.device = index:0

# the other one
[band:20m]
source = module
module = rtlsdr
`;
  it('rewrites the line that holds it and keeps the comments', () => {
    const next = setSectionValue(bands, 'band:40m', 'module.device', 'serial:00000001');
    expect(next).toBe(bands.replace('module.device = index:0', 'module.device = serial:00000001'));
  });
  it('adds it after the last setting when the section has none', () => {
    const next = setSectionValue(bands, 'band:20m', 'module.device', 'serial:00000002');
    expect(next.endsWith('module = rtlsdr\nmodule.device = serial:00000002\n')).toBe(true);
    expect(readSection(next, 'band:20m')?.get('module.device')).toBe('serial:00000002');
    const first = setSectionValue(bands, 'band:40m', 'gain', '30');
    expect(first).toContain('module.device = index:0\ngain = 30\n\n# the other one');
  });
  it('leaves a file without the section alone', () => {
    expect(setSectionValue(bands, 'band:10m', 'x', 'y')).toBe(bands);
  });
});
