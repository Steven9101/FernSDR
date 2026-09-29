/**
 * A backup file, as `/api/admin/backup` makes it and `/api/admin/restore`
 * takes it back: read here first, so a wrong file is named before anything
 * is sent, and the operator sees whose receiver it is.
 */

export interface BackupSummary {
  station: string;
  version: string;
  /** ms since the epoch; 0 when the file does not say. */
  created: number;
  bands: number;
  pictures: number;
  modules: string[];
}

export function readBackup(text: string): BackupSummary | null {
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch {
    return null;
  }
  if (typeof parsed !== 'object' || parsed === null) return null;
  const backup = parsed as Record<string, any>;
  const config = backup.files?.['fernsdr.conf'];
  if (backup.fernsdr_backup !== 1 || typeof config !== 'string') return null;
  return {
    station: typeof backup.station === 'string' && backup.station ? backup.station : 'FernSDR',
    version: typeof backup.version === 'string' ? backup.version : '',
    created: typeof backup.created_ms === 'number' ? backup.created_ms : 0,
    bands: (config.match(/^\s*\[band:[^\]]+\]/gm) ?? []).length,
    pictures: Array.isArray(backup.pictures) ? backup.pictures.length : 0,
    modules: Array.isArray(backup.modules)
      ? backup.modules.map((module: { id?: unknown }) => module?.id).filter((id: unknown): id is string => typeof id === 'string')
      : [],
  };
}

/** "fernsdr-backup-riverside-websdr-2026-09-29.json" */
export function backupFileName(station: string, date: Date): string {
  const slug = station.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 40);
  const day = `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`;
  return `fernsdr-backup-${slug ? `${slug}-` : ''}${day}.json`;
}
