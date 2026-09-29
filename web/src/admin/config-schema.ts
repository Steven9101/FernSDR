/**
 * What may appear in `config.ini`, so the editor can highlight it and complete
 * it.
 *
 * The server reads its configuration ad hoc: every setting is a `get("...")`
 * at the point it is needed, which is a fine way to write it and leaves no
 * list anywhere. This is that list, written out once, and `config-schema.test`
 * greps the server for every key it reads and fails if one is missing here.
 * So the drift this file invites is caught by the thing that would suffer
 * from it.
 */

export interface Setting {
  key: string;
  /** What it does, in one line. Shown beside the completion. */
  detail: string;
  /** What a value looks like, for the placeholder a completion inserts. */
  example?: string;
  /** Values worth offering directly. */
  values?: string[];
}

export interface Section {
  /** The header, or the prefix for a family like `band:20m`. */
  name: string;
  prefixed?: boolean;
  detail: string;
  settings: Setting[];
}

export const SECTIONS: Section[] = [
  {
    name: 'site',
    detail: 'Who this receiver belongs to, and what listeners are told',
    settings: [
      { key: 'name', detail: 'Shown in the top bar and in listings', example: 'Example WebSDR' },
      { key: 'operator', detail: 'Your callsign', example: 'N0CALL' },
      { key: 'location', detail: 'Where the antenna is', example: 'Somewhere, Earth' },
      { key: 'grid', detail: 'Maidenhead locator', example: 'DN31' },
      {
        key: 'band_plan',
        detail: 'Band plan drawn over the waterfall: auto works out the ITU region from grid; r1, r2, r3 for a region; a country for its national plan; none for no plan',
        values: ['auto', 'r1', 'r2', 'r3', 'us', 'ca', 'gb', 'de', 'au', 'jp', 'none'],
      },
      { key: 'antenna', detail: 'What it is listening on', example: '80 m horizontal loop at 12 m' },
      { key: 'contact', detail: 'Email or a page people can reach you on' },
      { key: 'website', detail: 'A page about the station', example: 'https://example.org' },
      { key: 'notice', detail: 'A banner shown to everyone. Empty for none' },
      { key: 'max_users', detail: 'Beyond this, new listeners are told it is full', example: '200' },
      { key: 'listener_timeout', detail: 'Minutes without activity before a listener is asked and then let go; 0 for never', example: '60' },
      { key: 'max_users_per_address', detail: 'Listeners from one IPv4 address or IPv6 /64; 0 for no limit', example: '16' },
      { key: 'chat', detail: 'Whether listeners may chat; no refuses messages and keeps none', values: ['yes', 'no'] },
      { key: 'sdr_list', detail: 'List this receiver on sdr-list.xyz; needs grid and public_host', values: ['yes', 'no'] },
      { key: 'public_host', detail: 'The address listeners use, for the directory listing', example: 'sdr.example.org' },
      { key: 'public_port', detail: "The port listeners use, if not the receiver's own", example: '8073' },
      { key: 'source_url', detail: 'Where the source for this receiver lives' },
      { key: 'spot_server', detail: "Where spots are reported, host:port; PSK Reporter's test port is 14739", example: 'report.pskreporter.info:4739' },
      { key: 'theme_file', detail: 'Where the appearance is stored. Set only in the file on the machine' },
    ],
  },
  {
    name: 'server',
    detail: 'The socket, and what is served over it. Set only in the file on the machine, and applied by a restart',
    settings: [
      { key: 'bind', detail: 'Address to listen on. 0.0.0.0 is everything', example: '0.0.0.0' },
      { key: 'port', detail: 'TCP port', example: '8073' },
      { key: 'root', detail: 'The built client. Relative paths are read from this file', example: '../web/dist' },
      { key: 'uploads', detail: 'Where uploaded images are kept, beside this file', example: 'fernsdr-uploads' },
      { key: 'websocket_path', detail: 'Where the stream lives', example: '/ws' },
      { key: 'max_connections', detail: 'Sockets accepted at once', example: '400' },
      {
        key: 'max_connections_per_address',
        detail: 'Sockets one address may hold at once; 0 is no limit. Not applied to trusted_proxies',
        example: '32',
      },
      { key: 'dsp_workers', detail: 'Shared audio workers, 0 to 32. Unset chooses from available CPUs', example: '2' },
      { key: 'idle_timeout', detail: 'Seconds before a silent connection is closed', example: '120' },
      { key: 'trusted_proxies', detail: 'Whose X-Forwarded-For to believe', values: ['loopback', 'none'] },
      { key: 'frame_ancestors', detail: 'Sites that may show the page in a frame: self, none or origins', values: ['self', 'none'] },
      { key: 'log_level', detail: 'How much to write', values: ['debug', 'info', 'warn', 'error'] },
    ],
  },
  {
    name: 'band',
    prefixed: true,
    detail: 'One radio and the stretch of spectrum it covers',
    settings: [
      { key: 'name', detail: 'What listeners see', example: '20 m' },
      { key: 'hours', detail: 'When the band is on the air, in UTC or by the Sun at the station. Bands on one input take turns', example: 'sunset-sunrise' },
      { key: 'source', detail: 'Where the samples come from', values: ['test', 'stdin', 'file', 'udp', 'module'] },
      { key: 'module', detail: 'For source = module: the installed module that runs the hardware', example: 'rtlsdr' },
      { key: 'path', detail: 'For source = file: the file or FIFO to read. Set only in the file on the machine' },
      { key: 'format', detail: 'Sample format', values: ['cu8', 'cs16', 'cf32', 's16', 'f32'] },
      { key: 'signal', detail: 'Complex or real input', values: ['iq', 'real'] },
      { key: 'sample_rate', detail: 'What you told the radio. Accepts k and M', example: '2048k' },
      { key: 'center', detail: 'Centre frequency. Accepts k and M', example: '14.2M' },
      { key: 'frequency_offset', detail: 'Correction for a converter ahead of the radio' },
      { key: 'ppm', detail: "The radio's crystal error, in parts per million; the panel can measure it", example: '0' },
      { key: 'iq_swap', detail: 'I and Q exchanged: for an input that mirrors the spectrum', values: ['yes', 'no'] },
      { key: 'dc_remove', detail: 'Take out the spike in the middle of a zero-IF receiver', values: ['yes', 'no'] },
      { key: 'iq_balance', detail: 'Correct I against Q, which suppresses mirror images', values: ['yes', 'no'] },
      { key: 'usable_fraction', detail: 'How much of the band is free of the front end roll-off' },
      { key: 'max_bandwidth', detail: 'Widest passband one listener may ask for', example: '12k' },
      { key: 'wfm', detail: 'Offer broadcast FM, where the band is sampled at 240 kHz or more', values: ['yes', 'no'] },
      { key: 'max_user_bitrate', detail: 'Ceiling on one listener, audio and waterfall together', example: '100000' },
      { key: 'audio_bitrate', detail: 'The SSB figure. Other modes scale from it', example: '48000' },
      { key: 'noise_blanker', detail: 'Impulse blanking, 0 to 1. 0 is off', example: '0' },
      { key: 'calibration', detail: 'dBFS to dBm, per frequency', example: '14.1M:-23.5, 28M:-29' },
      { key: 'history', detail: 'Keep a rolling waterfall archive, and who may read it', values: ['off', 'private', 'public'] },
      { key: 'history_hours', detail: 'How long the archive keeps. Changing it starts it again', example: '24' },
      { key: 'history_bins', detail: 'Archive width in bins', example: '1024' },
      { key: 'history_interval', detail: 'Seconds between archived lines', example: '1' },
      { key: 'history_path', detail: 'Where the archive file lives. Set only in the file on the machine' },
      { key: 'fft_size', detail: 'Transform size. Chosen from the sample rate if unset' },
      { key: 'spectrum_bins', detail: 'Waterfall resolution' },
      { key: 'spectrum_rate', detail: 'Waterfall lines a second', example: '25' },
      { key: 'spectrum_averages', detail: 'Transforms folded into one line' },
      { key: 'spectrum_smoothing', detail: 'How much a line is smoothed into the last' },
      { key: 'bind', detail: 'For source = udp: the local address to receive on. Set only in the file on the machine' },
      { key: 'multicast', detail: 'For source = udp: the group to join. Set only in the file on the machine' },
      {
        key: 'senders',
        detail: 'For source = udp: addresses whose samples are taken, e.g. loopback, 192.168.1.20. Empty takes everyone. Set only in the file on the machine',
      },
      { key: 'port', detail: 'For source = udp: the port to receive on. Set only in the file on the machine' },
      { key: 'rtp', detail: 'For source = udp: the stream carries RTP headers', values: ['true', 'false'] },
      { key: 'loop', detail: 'For source = file: start again at the end', values: ['true', 'false'] },
      { key: 'realtime', detail: 'For source = file: feed at the real sample rate', values: ['true', 'false'] },
      { key: 'noise', detail: 'For source = test: how much noise the synthetic band has' },
    ],
  },
  {
    name: 'decoder',
    prefixed: true,
    detail: 'A decoder module listening to channels of the bands, such as FT8',
    settings: [
      { key: 'module', detail: "The decoder module's id", example: 'ft8' },
      { key: 'channels', detail: 'Band and dial frequency of each channel', example: '40m:7074000 20m:14074000' },
      { key: 'mode', detail: 'What the channels carry', example: 'ft8' },
      { key: 'width', detail: 'How much audio each channel covers above its dial', example: '4000' },
      { key: 'offset', detail: 'Where the channel is centred above the dial; half the width unless set', example: '2000' },
      { key: 'public', detail: 'Show its decodes to listeners; no keeps them for the panel', values: ['yes', 'no'] },
      { key: 'report', detail: "Report its spots to PSK Reporter under the station's callsign and locator", values: ['none', 'pskreporter'] },
    ],
  },
  {
    name: 'modules',
    detail: 'Hardware modules. Set only in the file on the machine',
    settings: [
      { key: 'directory', detail: 'Where installed modules live, beside this file', example: 'fernsdr-modules' },
      { key: 'catalog', detail: 'GitHub repositories the panel may install from, owner/name', example: 'Steven9101/Fern-RTLSDR Steven9101/Fern-RX888 Steven9101/Fern-SDRPlay Steven9101/Fern-FT8' },
    ],
  },
  {
    name: 'admin',
    detail: 'The control panel. Set only in the file on the machine',
    settings: [
      { key: 'enabled', detail: 'Whether the panel exists at all', values: ['true', 'false'] },
      { key: 'password_hash', detail: 'Kept on the machine: set it there with fernsdr --hash-password' },
      { key: 'password', detail: 'Deprecated. Use password_hash' },
      { key: 'session_hours', detail: 'How long a sign-in lasts', example: '8' },
      {
        key: 'plain_http_anywhere',
        detail: 'At your own risk: plain HTTP from the internet too. Anyone on the path can read the panel and catch the password',
        values: ['no', 'yes'],
      },
      {
        key: 'home_network',
        detail: 'Plain HTTP from the home network too, not only from this machine',
        values: ['yes', 'no'],
      },
    ],
  },
];

/** The section a line belongs to, by looking back for the nearest header. */
export function sectionAt(text: string, caret: number): Section | null {
  const before = text.slice(0, caret);
  let found: Section | null = null;
  for (const line of before.split('\n')) {
    const header = /^\s*\[\s*([a-z]+)(?::[^\]]*)?\s*\]/i.exec(line);
    if (!header) continue;
    const name = header[1].toLowerCase();
    found = SECTIONS.find((s) => s.name === name) ?? null;
  }
  return found;
}

/** Settings matching what has been typed so far, best prefix matches first. */
export function completionsFor(section: Section | null, typed: string): Setting[] {
  const pool = section ? section.settings : SECTIONS.flatMap((s) => s.settings);
  const needle = typed.trim().toLowerCase();
  if (!needle) return pool.slice(0, 12);
  const starts = pool.filter((s) => s.key.startsWith(needle));
  const contains = pool.filter((s) => !s.key.startsWith(needle) && s.key.includes(needle));
  return [...starts, ...contains].slice(0, 12);
}
