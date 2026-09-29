import { describe, expect, it } from 'vitest';
import { backupFileName, readBackup } from './backup';

describe('backup files', () => {
  it('reads what the receiver wrote', () => {
    const text = JSON.stringify({
      fernsdr_backup: 1,
      version: '0.1.0',
      created_ms: 1790000000000,
      station: 'Riverside WebSDR',
      files: { 'fernsdr.conf': '[site]\nname = x\n[band:40m]\ncenter = 7.1M\n  [band:20m]\ncenter = 14.1M\n' },
      pictures: [{ name: 'a.png', data: '' }],
      modules: [{ id: 'rtlsdr', version: '1.0.0', origin: 'Steven9101/Fern-RTLSDR' }],
    });
    expect(readBackup(text)).toEqual({
      station: 'Riverside WebSDR',
      version: '0.1.0',
      created: 1790000000000,
      bands: 2,
      pictures: 1,
      modules: ['rtlsdr'],
    });
  });

  it('turns away what is not a backup', () => {
    expect(readBackup('not json')).toBeNull();
    expect(readBackup('null')).toBeNull();
    expect(readBackup('{"theme":{}}')).toBeNull();
    expect(readBackup('{"fernsdr_backup":2,"files":{"fernsdr.conf":""}}')).toBeNull();
    expect(readBackup('{"fernsdr_backup":1,"files":{}}')).toBeNull();
  });

  it('names the file after the station and the day', () => {
    expect(backupFileName('Riverside WebSDR', new Date(2026, 8, 29))).toBe('fernsdr-backup-riverside-websdr-2026-09-29.json');
    expect(backupFileName('', new Date(2026, 0, 5))).toBe('fernsdr-backup-2026-01-05.json');
  });
});
