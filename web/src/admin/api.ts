import { fromHex, hmacSha256, pbkdf2Sha256, toHex, utf8 } from './crypto';
import type { Tuning } from './lib/band-suggestions';

/**
 * The key this session signs its changes with, kept so every change can be
 * signed without the password.
 *
 * It is made at sign-in from the password's derived key and this session's
 * signing context, and the derived key itself is dropped: that one signs in
 * by itself and never expires, so a script or an extension that read it from
 * the page would hold the receiver until the password changed. This one is
 * worth nothing once the session ends.
 *
 * In `sessionStorage` rather than only in memory, so a reload does not force
 * the password to be typed again. It is per tab and gone when the tab closes.
 */
const KEY_STORAGE = 'fernsdr_admin_key_v3';
// Keys from before, which were the password's derived key itself: dropped as
// soon as this page loads rather than left until the tab closes.
try {
  sessionStorage.removeItem('fernsdr_admin_key_v2');
  sessionStorage.removeItem('fernsdr_admin_key');
} catch {
  // Storage turned off; nothing was kept then either.
}
let signingKey: Uint8Array | null = null;
let signingContext = '';
// Persist the counter with the key: a reload must still advance it if NTP
// moved the wall clock backwards in the meantime.
//
// It is the clock in microseconds, or one more than the last if that is
// later. The server takes a counter only above the last one it saw for the
// login, and every tab of one login shares that: a tab counting up from the
// moment it loaded fell behind another tab loaded later, and after that tab's
// first change every change it made was refused as unsigned. From the clock,
// the tabs' counters rise in the order the changes are made.
let counter = Date.now() * 1000;

function nextCounter(): string {
  counter = Math.max(counter + 1, Date.now() * 1000);
  return String(counter);
}

function rememberKey(key: Uint8Array, context: string): void {
  signingKey = key;
  signingContext = context;
  persistKey();
}

function persistKey(): void {
  if (!signingKey) return;
  try {
    sessionStorage.setItem(KEY_STORAGE, JSON.stringify({ key: toHex(signingKey), context: signingContext, counter }));
  } catch {
    // Private browsing, or storage turned off. Signing still works for this
    // page load; a reload will ask for the password again.
  }
}

function currentKey(): Uint8Array | null {
  if (signingKey) return signingKey;
  try {
    const stored = sessionStorage.getItem(KEY_STORAGE);
    if (stored) {
      const value = JSON.parse(stored);
      if (/^[a-f0-9]{64}$/.test(value.key) && /^[a-f0-9]{64}$/.test(value.context)) {
        signingKey = fromHex(value.key);
        signingContext = value.context;
        if (Number.isSafeInteger(value.counter) && value.counter >= 0 && value.counter < Number.MAX_SAFE_INTEGER) {
          counter = Math.max(counter, value.counter);
        }
      }
    }
  } catch {
    // As above.
  }
  return signingKey;
}

/**
 * Whether this tab can sign changes. The session cookie outlives the tab and the key does not, so
 * a panel opened in a new tab can read but not save, and should ask for the password rather than
 * look signed in and then fail every change.
 */
export function canSign(): boolean {
  return currentKey() !== null;
}

export function forgetKey(): void {
  signingKey = null;
  signingContext = '';
  try {
    sessionStorage.removeItem(KEY_STORAGE);
    sessionStorage.removeItem('fernsdr_admin_key_v2');
    sessionStorage.removeItem('fernsdr_admin_key');
  } catch {
    // Nothing to remove.
  }
}

/**
 * The admin API, and the one rule that governs it: the browser never holds the
 * session.
 *
 * The token lives in an HttpOnly cookie the server set, so nothing here can
 * read it, store it, or leak it through an injected script. This module just
 * makes requests with `credentials: 'same-origin'` and reads the answers.
 */

export interface BandState {
  id: string;
  name: string;
  low: number;
  high: number;
  listeners: number;
  /** The band's thread is running; see `online` for whether samples arrive. */
  running: boolean;
  /** Samples are arriving. A module band waiting to try again is running but not online. */
  online?: boolean;
  /** Why the band is not online, in the server's words; empty when it is. */
  status?: string;
  /** Seconds until a failed input is tried again; absent when nothing is scheduled. */
  retry_in?: number;
  /** Whether the band's hours have it on the air; off the air it is stopped on purpose. */
  on_air?: boolean;
  /** The band's hours as the configuration has them, `always` without any. */
  hours?: string;
  /** When on_air next changes, UTC ms; -1 for not in the next days. */
  next_change?: number;
  /** The level between the signals, in dBFS; -160 before the first line. */
  noise_floor: number;
  restarting: boolean;
  error: string;
  /** Present when the band's input is a module. */
  module?: ModuleRun;
  /** What the front end corrections are finding, while they run. */
  input?: {
    dc_offset_dbfs?: number;
    gain_error_db?: number;
    phase_error_degrees?: number;
    image_rejection_db?: number;
  };
}

/** A setting a running module can change without a restart. */
export interface LiveSetting {
  key: string;
  type: 'string' | 'number' | 'boolean' | 'choice';
  label: string;
  unit?: string;
  min?: number;
  max?: number;
  choices?: string[];
}

type SettingValue = string | number | boolean;

/** What a band's module is doing, as the server last saw it. */
export interface ModuleRun {
  module: string;
  version: string;
  state: 'idle' | 'starting' | 'streaming' | 'failed' | 'stopped' | 'disabled';
  pid?: number;
  device?: Record<string, SettingValue>;
  settings?: Record<string, SettingValue>;
  /** Where the module says the hardware tuned, Hz: while streaming, within 10 Hz of the band's centre. */
  center?: number;
  /** The share of the samples at the converter's limit between the module's last two stats, when it counts them. */
  clipping?: number;
  /** The gain in dB the module is using, when it sets the gain itself. */
  gain_now?: number;
  samples: number;
  module_samples: number;
  dropped: number;
  failure?: string;
  exit?: string;
  live: LiveSetting[];
  last_set?: { id: number; ok: boolean; message?: string; settings?: Record<string, SettingValue> };
  /** The latest lines only; the whole log comes from `moduleLog`. */
  log: string[];
}

export interface ModuleSettingSpec extends LiveSetting {
  help?: string;
  live?: boolean;
  /** Most operators should leave it alone; shown apart from the rest. */
  advanced?: boolean;
  default?: SettingValue;
}

export interface ModuleVersion {
  id: string;
  name: string;
  version: string;
  /** input brings samples in; decoder listens to channels. Absent from older receivers. */
  kind?: 'input' | 'decoder';
  platform: string;
  size: number;
  sha256: string;
  license: string;
  source: string;
  description: string;
  /** Only filled in for the active version. */
  settings: ModuleSettingSpec[];
  /** What the operator installs first that the package cannot carry, such as a vendor's API. */
  requires?: string[];
  /** For an input module: what its radio can be set to, for the band suggestions. */
  tuning?: Tuning;
}

/** A radio on USB, seen before any module for it is installed. */
export interface UsbRadio {
  usb: string;
  port: string;
  name: string;
  /** The module that drives it; empty when there is none yet. */
  module: string;
  serial?: string;
}

export interface HardwareView {
  radios: UsbRadio[];
  /** Kernel TV drivers holding a radio, which a restart of the machine lets go. */
  drivers: { driver: string; radio: string; module: string }[];
  can_restart: boolean;
  restart_note?: string;
}

export interface InstalledModule {
  id: string;
  active: string;
  enabled: boolean;
  origin: string;
  versions: ModuleVersion[];
  /** Bands configured with this module. */
  bands: string[];
  /** A newer release from the repository it came from. */
  update?: { version: string; repository: string; tag: string; asset: string };
}

export interface CatalogRelease {
  tag: string;
  name: string;
  published: string;
  prerelease: boolean;
  /** The package for this receiver's platform; absent when the release has none. */
  asset?: string;
  size?: number;
  id?: string;
  version?: string;
}

export interface CatalogRepository {
  repository: string;
  fetched_ms?: number;
  error?: string;
  releases?: CatalogRelease[];
}

export interface ModuleDevice {
  index?: number;
  name?: string;
  serial?: string;
  tuner?: string;
  usable: boolean;
  error?: string;
}

export interface ModuleJob {
  number: number;
  kind: string;
  summary: string;
  state: 'queued' | 'running' | 'done' | 'failed';
  message?: string;
  finished_ms?: number;
}

export interface ModulesView {
  directory: string;
  platform: string;
  /** "curl", or empty when it is not installed. */
  downloader: string;
  catalog: string[];
  installed: InstalledModule[];
  available: CatalogRepository[];
  devices: Record<string, { fetched_ms: number; error?: string; devices: ModuleDevice[] }>;
  jobs: ModuleJob[];
}

/** What the Updates page shows: the running version and how moving on goes. */
export interface UpdateView {
  running: string;
  platform: string;
  /** Whether this receiver can update itself; `unavailable` says why not. */
  available: boolean;
  unavailable?: string;
  check: {
    state: 'idle' | 'checking' | 'done' | 'failed';
    /** Epoch seconds of the last look. */
    checked?: number;
    version?: string;
    date?: string;
    /** Release notes, plain text. */
    notes?: string;
    newer?: boolean;
    error?: string;
  };
  /** The updater's own account of the last or current update. */
  status?: UpdateStatus;
}

export interface UpdateStatus {
  state: 'checking' | 'downloading' | 'installing' | 'trial' | 'updated' | 'rolled-back' | 'refused' | 'failed';
  version: string;
  message: string;
  /** Epoch seconds. */
  time: number;
}

export interface ListenerState {
  id: number;
  address: string;
  band: string;
  frequency: number;
  mode: string;
  bandwidth: number;
  audio_bitrate: number;
  waterfall_bitrate: number;
  connected_seconds: number;
  /** Muted in the chat, by this address or the network it belongs to. */
  muted: boolean;
}

/** A picture uploaded through the panel. */
export interface UploadedPicture {
  name: string;
  url: string;
  bytes: number;
  /** Epoch milliseconds. */
  modified: number;
  /** Whether anything the operator has configured points at it. */
  in_use: boolean;
}

/** An address the operator has silenced in the chat, and until when. */
export interface MutedAddress {
  address: string;
  /** Epoch milliseconds; zero means until the operator lifts it. */
  until: number;
}

/** Reporting spots to PSK Reporter, for every decoder that does. */
export interface SpotStatus {
  enabled: boolean;
  /** Why it cannot report: the station's callsign or locator. */
  problem?: string;
  waiting: number;
  sent: number;
  last_sent_ms?: number;
  error?: string;
}

/** A running decoder, as /api/admin/decoders reports it. */
export interface DecoderStatus {
  id: string;
  module: string;
  /** What the module said it is; empty until it has said hello. */
  version: string;
  state: 'starting' | 'running' | 'waiting' | 'stopped' | string;
  message: string;
  restarts: number;
  rejected: number;
  last_rejection?: string;
  public: boolean;
  /** Its decodes go to PSK Reporter (report = pskreporter). */
  report?: boolean;
  channels: { id: string; band: string; mode: string; dial: number; frames_sent: number; frames_dropped: number; decodes: number }[];
  module_stats: { channels?: { id: string; slots?: number; decodes?: number; late?: number; cpu_ms?: number }[] };
  log: string[];
}

export interface AdminState {
  site: string;
  users: number;
  max_users: number;
  bands: BandState[];
  listeners: ListenerState[];
  muted?: MutedAddress[];
}

export class ApiError extends Error {
  constructor(message: string, readonly status: number, readonly retryAfter = 0) {
    super(message);
  }
}

let pendingWrite: Promise<unknown> = Promise.resolve();

/**
 * How long a change waits for the one before it. A request whose answer
 * never finishes arriving would otherwise hold every later save, sign-in and
 * sign-out until the page is reloaded. Past this the next one goes anyway:
 * should the stalled one reach the receiver after it, its counter is refused
 * and it is signed again like any other clash.
 */
export const WRITE_TURN_MS = 20_000;

function turn(previous: Promise<unknown>): Promise<void> {
  return new Promise((resolve) => {
    const timer = setTimeout(resolve, WRITE_TURN_MS);
    void previous.then(() => {
      clearTimeout(timer);
      resolve();
    });
  });
}

function call<T>(path: string, options: RequestInit = {}): Promise<T> {
  if ((options.method ?? 'GET').toUpperCase() === 'GET') return request<T>(path, options);
  // The server accepts counters in order. Serialise mutations so two quick
  // edits cannot arrive in reverse order and reject the earlier edit.
  // Two tabs can still meet within one microsecond of the clock. The refusal
  // is then about the counter, not the login: signed again with a new one,
  // the change goes through; a second refusal is real and is shown.
  const attempt = () => request<T>(path, options);
  const result = turn(pendingWrite).then(() =>
    attempt()
      .catch((problem: unknown) => {
        if (problem instanceof ApiError && problem.status === 401 && /not signed/.test(problem.message) && currentKey())
          return attempt();
        throw problem;
      })
      .catch((problem: unknown) => {
        // Refused twice, or the session is gone: this tab's key belongs to a
        // login that has ended, as when another tab signed in and the shared
        // cookie moved to its login. Reading still works, so without this the
        // panel would look signed in and fail every save. It signs in again.
        if (problem instanceof ApiError && problem.status === 401 && /not signed|sign in first/.test(problem.message)) {
          forgetKey();
          signedOut?.();
        }
        throw problem;
      }),
  );
  pendingWrite = result.catch(() => {});
  return result;
}

let signedOut: (() => void) | null = null;

/** Called when a change is refused because this tab's login has ended. */
export function whenSignedOut(handler: (() => void) | null): void {
  signedOut = handler;
}

async function request<T>(path: string, options: RequestInit): Promise<T> {
  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
    ...((options.headers as Record<string, string>) ?? {}),
  };

  // The public context binds a signature to one login without exposing its
  // HttpOnly bearer cookie. Remote administration also requires HTTPS.
  const method = (options.method ?? 'GET').toUpperCase();
  if (method !== 'GET') {
    const key = currentKey();
    if (key) {
      const count = nextCounter();
      persistKey();
      // The body is covered whatever it is. A binary upload signed over an
      // empty body would still be bound to its method, path and counter, but
      // the bytes themselves would be free to swap - which for an image the
      // page then shows to everybody is exactly the thing worth binding.
      const prefix = utf8(`fernsdr-admin-v3\n${signingContext}\n${count}\n${method}\n${path}\n`);
      const body =
        typeof options.body === 'string'
          ? utf8(options.body)
          : options.body instanceof Uint8Array
            ? options.body
            : options.body instanceof ArrayBuffer
              ? new Uint8Array(options.body)
              : new Uint8Array(0);
      const message = new Uint8Array(prefix.length + body.length);
      message.set(prefix);
      message.set(body, prefix.length);
      headers['X-FernSDR-Counter'] = count;
      headers['X-FernSDR-Signature'] = toHex(hmacSha256(key, message));
    }
  }

  const response = await fetch(path, { ...options, credentials: 'same-origin', headers });

  let payload: any = null;
  try {
    payload = await response.json();
  } catch {
    // A non-JSON body from an admin endpoint means something upstream - a
    // proxy, an error page - answered instead of the receiver.
  }

  if (!response.ok) {
    throw new ApiError(payload?.error ?? `request failed (${response.status})`, response.status,
                       payload?.retry_after ?? 0);
  }
  return payload as T;
}

/** What the stored hash of a new password is made with; OWASP's current figure, as the receiver's own tool uses. */
const NEW_PASSWORD_ROUNDS = 600000;

/**
 * PBKDF2-SHA256 of `password` with the salt's hex text as its bytes, as the
 * receiver derives it. Web Crypto derives off the UI thread; the local
 * fallback is for pages over plain HTTP, where browsers withhold it, and
 * uses the same published vectors.
 */
async function deriveKey(password: string, salt: string, iterations: number): Promise<Uint8Array> {
  const subtle = globalThis.crypto?.subtle;
  return subtle
    ? new Uint8Array(await subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256',
        salt: new TextEncoder().encode(salt), iterations },
        await subtle.importKey('raw', new TextEncoder().encode(password), 'PBKDF2', false, ['deriveBits']), 256))
    : pbkdf2Sha256(utf8(password), utf8(salt), iterations, 32);
}

export const api = {
  /** `exposed`: let in over plain HTTP from outside only because of [admin] plain_http_anywhere. */
  session: () => call<{ authorised: boolean; exposed?: boolean }>('/api/admin/session'),
  /**
   * Signs in without sending the password.
   *
   * The server hands back the salt and iteration count its stored hash was
   * made with, plus a single-use nonce. This side derives the same key and
   * returns HMAC(key, nonce), so what crosses the wire proves knowledge of the
   * password without running an expensive password derivation on the server's
   * I/O thread. Remote administration requires HTTPS for the whole session.
   *
   * The derivation is deliberately slow, so `onProgress` gives the caller
   * something to say while it runs.
   */
  login: async (password: string, onProgress?: () => void): Promise<{ ok: boolean }> => {
    const challenge = await call<{ salt: string; iterations: number; nonce: string }>(
      '/api/admin/challenge',
      { method: 'POST' },
    );
    onProgress?.();
    // The server uses the salt's hex text as the salt bytes, not the decoded
    // value; deriving from the decoded bytes produces a different key and a
    // proof the server will reject.
    if (!Number.isInteger(challenge.iterations) || challenge.iterations < 1 || challenge.iterations > 2_000_000 ||
        !/^[a-f0-9]{32}$/.test(challenge.nonce) || !/^[a-f0-9]{16,128}$/.test(challenge.salt)) {
      throw new ApiError('Invalid sign-in challenge', 400);
    }
    const key = await deriveKey(password, challenge.salt, challenge.iterations);
    const proof = toHex(hmacSha256(key, utf8(challenge.nonce)));
    const result = await call<{ ok: boolean; signing_context: string }>('/api/admin/login', {
      method: 'POST',
      body: JSON.stringify({ nonce: challenge.nonce, proof }),
    });
    if (!/^[a-f0-9]{64}$/.test(result.signing_context)) throw new ApiError('Reload this page to sign in', 401);
    rememberKey(hmacSha256(key, utf8(`fernsdr-admin-session-v3\n${result.signing_context}`)), result.signing_context);
    key.fill(0);
    return result;
  },
  /**
   * A new password, as sign-in works: neither password crosses the network.
   * The current one proves itself against a fresh challenge; the new one
   * becomes a stored hash here, with a salt drawn here, and only the hash
   * goes. The receiver ends every session on the change, so this signs in
   * again with the new password before it returns.
   */
  changePassword: async (current: string, next: string, onProgress?: (step: string) => void): Promise<void> => {
    onProgress?.('Checking the current password…');
    const challenge = await call<{ salt: string; iterations: number; nonce: string }>(
      '/api/admin/challenge',
      { method: 'POST' },
    );
    if (!Number.isInteger(challenge.iterations) || challenge.iterations < 1 || challenge.iterations > 2_000_000 ||
        !/^[a-f0-9]{32}$/.test(challenge.nonce) || !/^[a-f0-9]{16,128}$/.test(challenge.salt)) {
      throw new ApiError('Invalid sign-in challenge', 400);
    }
    const currentKey = await deriveKey(current, challenge.salt, challenge.iterations);
    const proof = toHex(hmacSha256(currentKey, utf8(challenge.nonce)));
    currentKey.fill(0);
    onProgress?.('Making the new password…');
    const salt = toHex(globalThis.crypto.getRandomValues(new Uint8Array(16)));
    const derived = await deriveKey(next, salt, NEW_PASSWORD_ROUNDS);
    const hash = `pbkdf2$${NEW_PASSWORD_ROUNDS}$${salt}$${toHex(derived)}`;
    derived.fill(0);
    await call<{ ok: boolean }>('/api/admin/password', {
      method: 'POST',
      body: JSON.stringify({ nonce: challenge.nonce, proof, hash }),
    });
    forgetKey();
    onProgress?.('Signing in with the new password…');
    await api.login(next);
  },
  logout: async () => {
    // The key goes whatever the receiver answers: a sign-out that failed on
    // the way must not leave this browser able to sign straight back in.
    try {
      return await call<{ ok: boolean }>('/api/admin/logout', { method: 'POST' });
    } finally {
      forgetKey();
    }
  },
  state: () => call<AdminState>('/api/admin/state'),
  log: () => call<{ lines: string[] }>('/api/admin/log'),
  readStation: () => call<{ station: unknown }>('/api/admin/station'),
  readDirectory: () =>
    call<{ enabled: boolean; state: string; detail: string; listed_ms: number }>('/api/admin/directory'),
  writeStation: (station: unknown) =>
    call<{ ok: boolean; station: unknown }>('/api/admin/station', {
      method: 'POST',
      body: JSON.stringify({ station }),
    }),
  readServer: () => call<{ server: Record<string, unknown>; config_path: string }>('/api/admin/server'),
  readBands: () => call<{ bands: unknown }>('/api/admin/bands'),
  writeBand: (id: string, values: unknown) =>
    call<{ ok: boolean; bands: unknown }>('/api/admin/bands', {
      method: 'POST',
      body: JSON.stringify({ id, values }),
    }),
  /** What the receiver reads right now at one frequency, in dBFS. */
  level: (band: string, hz: number, width = 1000) =>
    call<{ hz: number; dbfs: number }>(
      `/api/admin/level?band=${encodeURIComponent(band)}&hz=${Math.round(hz)}&width=${Math.round(width)}`,
    ),
  /**
   * Sends an image and gets back the path it is served from.
   *
   * The file goes as raw bytes rather than as a form: there is one file, it
   * needs no name (the server names it by its content) and no fields beside
   * it, and multipart would be a parser on the server for no benefit.
   */
  upload: async (file: File) => {
    const bytes = new Uint8Array(await file.arrayBuffer());
    return call<{ url: string }>('/api/admin/upload', {
      method: 'POST',
      body: bytes,
      headers: { 'Content-Type': 'application/octet-stream' },
    });
  },
  readTheme: () => call<{ theme: unknown }>('/api/admin/theme'),
  writeTheme: (theme: unknown) =>
    call<{ ok: boolean; theme: unknown }>('/api/admin/theme', {
      method: 'POST',
      body: JSON.stringify({ theme }),
    }),
  resetTheme: () =>
    call<{ ok: boolean; theme: unknown }>('/api/admin/theme', {
      method: 'POST',
      body: JSON.stringify({ reset: true }),
    }),
  readConfig: () => call<{ path: string; version: string; text: string }>('/api/admin/config'),
  /** `changes` says what a restart would apply, per band and per setting. */
  writeConfig: (text: string) =>
    call<{
      ok: boolean;
      applied: boolean;
      bands_changed: string[];
      /** Decoders started or stopped by the change; they apply at once. */
      decoders_changed?: string[];
      /** Decoders left as they were until a restart, by id, with why. */
      decoders_waiting?: Record<string, string>;
      /** `band_restart`: restarting the band applies it; otherwise the receiver must restart. */
      changes?: { band: string; key: string; running: string; configured: string; band_restart?: boolean }[];
    }>('/api/admin/config', {
      method: 'POST',
      body: JSON.stringify({ text }),
    }),
  decoders: () => call<{ decoders: DecoderStatus[]; kept: number; spots?: SpotStatus }>('/api/admin/decoders'),
  restartDecoder: (id: string) =>
    call<{ ok: boolean }>('/api/admin/decoders/restart', { method: 'POST', body: JSON.stringify({ id }) }),
  restartBand: (band: string) =>
    call<{ ok: boolean; note?: string }>('/api/admin/restart-band', {
      method: 'POST',
      body: JSON.stringify({ band }),
    }),
  uploads: () => call<{ uploads: UploadedPicture[]; bytes: number }>('/api/admin/uploads'),

  deleteUpload: (name: string) =>
    call<{ uploads: UploadedPicture[]; bytes: number }>('/api/admin/uploads/delete', {
      method: 'POST',
      body: JSON.stringify({ name }),
    }),

  /** Silences an address in the chat. `minutes` of zero is until it is lifted. */
  mute: (address: string, minutes: number) =>
    call<{ muted: MutedAddress[] }>('/api/admin/mute', {
      method: 'POST',
      body: JSON.stringify({ address, minutes }),
    }),

  unmute: (address: string) =>
    call<{ muted: MutedAddress[] }>('/api/admin/unmute', {
      method: 'POST',
      body: JSON.stringify({ address }),
    }),

  disconnect: (id: number) =>
    call<{ ok: boolean }>('/api/admin/disconnect', {
      method: 'POST',
      body: JSON.stringify({ id }),
    }),

  // Updates. A look runs on its own thread and the view says when it is done;
  // the update itself is the updater's, whose account the view carries.
  updates: () => call<UpdateView>('/api/admin/update'),
  checkUpdates: () => call<UpdateView>('/api/admin/update/check', { method: 'POST', body: '{}' }),
  startUpdate: (version: string) =>
    call<UpdateView>('/api/admin/update/start', { method: 'POST', body: JSON.stringify({ version }) }),

  // Modules. Every change is a job the server runs on its own thread; the
  // answer is the view with the job queued, and the panel polls for the rest.
  hardware: () => call<HardwareView>('/api/admin/hardware'),
  /** Everything needed to move this receiver to another machine, as one file. */
  backup: () => call<Record<string, unknown>>('/api/admin/backup'),
  /** Plays a backup file back; `modules` are the ones to install before the restart. */
  restore: (text: string) =>
    call<{ ok: boolean; modules: { id: string; origin: string }[]; pictures_not_restored: number }>('/api/admin/restore', {
      method: 'POST',
      body: text,
    }),
  /** Stops the receiver for its service to start it again; refused where nothing would. */
  restart: () => call<{ ok: boolean }>('/api/admin/restart', { method: 'POST', body: '{}' }),
  modules: () => call<ModulesView>('/api/admin/modules'),
  checkModules: () =>
    call<ModulesView>('/api/admin/modules/refresh', { method: 'POST', body: '{}' }),
  installModule: (repository: string, tag: string, asset: string, activate: boolean) =>
    call<ModulesView>('/api/admin/modules/install', {
      method: 'POST',
      body: JSON.stringify({ repository, tag, asset, activate }),
    }),
  activateModule: (id: string, version: string) =>
    call<ModulesView>('/api/admin/modules/activate', {
      method: 'POST',
      body: JSON.stringify({ id, version }),
    }),
  enableModule: (id: string, enabled: boolean) =>
    call<ModulesView>('/api/admin/modules/enable', {
      method: 'POST',
      body: JSON.stringify({ id, enabled }),
    }),
  removeModule: (id: string, version: string) =>
    call<ModulesView>('/api/admin/modules/remove', {
      method: 'POST',
      body: JSON.stringify({ id, version }),
    }),
  findDevices: (id: string) =>
    call<ModulesView>('/api/admin/modules/devices', {
      method: 'POST',
      body: JSON.stringify({ id }),
    }),
  /** Sends live settings to a band's running module; `applied` arrives in the band's state. */
  setModule: (band: string, settings: Record<string, SettingValue>) =>
    call<{ ok: boolean; sent: { id: number; settings: Record<string, SettingValue> } }>('/api/admin/modules/set', {
      method: 'POST',
      body: JSON.stringify({ band, settings }),
    }),
  moduleLog: (band: string) =>
    call<{ log: string[] }>(`/api/admin/modules/log?band=${encodeURIComponent(band)}`),
  /** Where the strongest carrier within `span` of `hz` shows on a band right now. */
  carrier: (band: string, hz: number, span = 2000) =>
    call<CarrierReading>(`/api/admin/carrier?band=${encodeURIComponent(band)}&hz=${hz}&span=${span}`),
  /** The band's spectrum now; with `recent`, also the last minute the band kept. */
  spectrum: (band: string, bins = 512, recent = false) =>
    call<SpectrumView>(
      `/api/admin/spectrum?band=${encodeURIComponent(band)}&bins=${bins}${recent ? '&recent=1' : ''}`,
    ),
};

export interface CarrierReading {
  found: boolean;
  /** Where it shows, on the band's corrected axis. */
  hz?: number;
  level_dbfs?: number;
  floor_dbfs: number;
  /** The correction in force now, and the converter offset it leaves alone. */
  ppm: number;
  frequency_offset: number;
}

export interface RecentSpectrum {
  /** Cells per line. */
  width: number;
  count: number;
  interval_ms: number;
  /** How long ago the newest line was taken; -1 with no lines. */
  age_ms: number;
  /** A byte of 0 is floor_db and 255 is ceiling_db, in even steps. */
  floor_db: number;
  ceiling_db: number;
  /** count lines of width bytes, oldest first, base64. */
  lines: string;
}

export interface SpectrumView {
  low?: number;
  high?: number;
  floor?: number;
  levels: number[];
  recent?: RecentSpectrum;
}

