/**
 * Binary stream layouts. Mirrors server/src/core/protocol.h.
 */

export const STREAM_AUDIO = 0x01;
export const STREAM_WATERFALL = 0x02;
export const STREAM_METER = 0x03;

export const AUDIO_HEADER_BYTES = 4;
export const WATERFALL_HEADER_BYTES = 22;

const AUDIO_FLAG_MUTED = 0x01;

export interface AudioPacket {
  compact?: boolean;
  /** A NAC3 packet of one to four frames; its sequence is its first frame's. */
  packet?: boolean;
  /** Earlier audio expired at the sender; resume without filling that gap. */
  discontinuity?: boolean;
  /** Wraps at 65536; used to spot dropped frames. */
  sequence: number;
  /** Squelch is closed. The payload is still a valid frame. */
  muted: boolean;
  /**
   * Which audio configuration this frame belongs to. The server announces
   * {generation -> sample rate} on the control channel; carrying the nibble
   * here instead of a sample rate saves 3 kbit/s at 94 frames a second.
   */
  generation: number;
  payload: Uint8Array;
}

export interface WaterfallPacket {
  zeroRuns?: boolean;
  adaptive?: boolean;
  nativeGrid?: boolean;
  stepDb?: 1 | 2;
  /** WFC5: the payload is a range-coded row (docs/CODEC.md). */
  rangeCoded?: boolean;
  sequence: number;
  /** The span this line covers, which travels with every line. */
  lowHz: number;
  highHz: number;
  width: number;
  payload: Uint8Array;
}

export type ServerBinary =
  | ({ kind: 'audio' } & AudioPacket)
  | ({ kind: 'waterfall' } & WaterfallPacket)
  | ({ kind: 'meter' } & MeterMessage);

/** Returns null for a message that is too short or of an unknown type. */
export function parseBinary(buffer: ArrayBuffer): ServerBinary | null {
  const bytes = new Uint8Array(buffer);
  if (bytes.length === 0) return null;

  const view = new DataView(buffer);
  switch (bytes[0]) {
    case STREAM_METER: {
      if (bytes.length !== 26 || (bytes[1] & 0xe0) !== 0) return null;
      const flags = bytes[1];
      return {
        kind: 'meter', type: 'meter',
        dbfs: view.getInt16(2, true) / 10,
        gain_db: view.getInt16(4, true) / 10,
        squelch_open: (flags & 1) !== 0,
        squelch_statistic: flags & 4 ? view.getUint16(6, true) / 10 : undefined,
        pll_locked: flags & 8 ? (flags & 2) !== 0 : undefined,
        pll_offset: flags & 8 ? view.getInt32(8, true) / 10 : undefined,
        // NFM's CTCSS tone rides in the same field (see "meter-ctcss").
        ctcss: flags & 16 ? view.getInt32(8, true) / 10 : undefined,
        audio_bps: view.getUint32(12, true),
        waterfall_bps: view.getUint32(16, true),
        waterfall_fps: view.getUint16(20, true) / 10,
        listeners: view.getUint32(22, true),
      };
    }
    case STREAM_AUDIO: {
      if (bytes.length < AUDIO_HEADER_BYTES) return null;
      const flags = bytes[1];
      const packet = (flags & 0x08) !== 0;
      // A NAC3 packet always carries its frame count, so it is never empty,
      // and it cannot also claim the NAC2 layout.
      if (packet && (bytes.length <= AUDIO_HEADER_BYTES || (flags & 0x02) !== 0)) return null;
      return {
        kind: 'audio',
        packet,
        compact: (flags & 2) !== 0,
        discontinuity: (flags & 4) !== 0,
        muted: (flags & AUDIO_FLAG_MUTED) !== 0,
        generation: (flags >> 4) & 0x0f,
        sequence: view.getUint16(2, true),
        payload: bytes.subarray(AUDIO_HEADER_BYTES),
      };
    }
    case STREAM_WATERFALL: {
      if (bytes.length < WATERFALL_HEADER_BYTES) return null;
      const lowHz = view.getFloat64(4, true);
      const highHz = view.getFloat64(12, true);
      const width = view.getUint16(20, true);
      const flags = bytes[1];
      if ((flags & ~31) !== 0 || !Number.isFinite(lowHz) || !Number.isFinite(highHz) ||
          highHz <= lowHz || width < (flags & 4 ? 2 : 16) || width > 4096) return null;
      return {
        kind: 'waterfall',
        zeroRuns: (bytes[1] & 1) !== 0,
        adaptive: (flags & 2) !== 0,
        nativeGrid: (flags & 4) !== 0,
        stepDb: flags & 8 ? 2 : 1,
        rangeCoded: (flags & 16) !== 0,
        sequence: view.getUint16(2, true),
        lowHz,
        highHz,
        width,
        payload: bytes.subarray(WATERFALL_HEADER_BYTES),
      };
    }
    default:
      return null;
  }
}

/** How far `sequence` is ahead of `previous`, accounting for the 16-bit wrap. */
export function sequenceGap(previous: number, sequence: number): number {
  return (sequence - previous - 1) & 0xffff;
}

// --- Control channel ---------------------------------------------------

/** One measured point: at `hz`, add `offset` to a dBFS reading to get dBm. */
export interface CalibrationPoint {
  hz: number;
  offset: number;
}

export interface BandDescription {
  /** What it listens with: a source kind ("file", "udp") or a module's id ("rx888"). */
  receiver?: string;
  id: string;
  name: string;
  center: number;
  low: number;
  high: number;
  sample_rate: number;
  sample_low?: number;
  sample_high?: number;
  max_bandwidth: number;
  /** Broadcast FM can be had here: the band is sampled wide enough and the operator allows it. */
  wfm?: boolean;
  listeners: number;
  running: boolean;
  /**
   * Whether the band's hours have it on the air. Off the air it is stopped on
   * purpose and its input, if shared, belongs to another band. Absent from
   * older receivers, which have no hours.
   */
  on_air?: boolean;
  /** The operator's hours, as `18:00-06:00` or `sunset-sunrise`; absent when always. */
  hours?: string;
  /** When on_air next changes, UTC ms; -1 for not in the next days. */
  next_change?: number;
  /** For bands taking turns on one input by their hours: the same id for all of them. */
  shared_input?: string;
  /** Level between the signals, dBFS. */
  noise_floor?: number;
  /** Whether this band keeps a waterfall archive, and who may read it. */
  history?: 'off' | 'private' | 'public';
  /** How far back the archive reaches, in wall-clock milliseconds. */
  history_from?: number;
  history_to?: number;
  /**
   * The operator's dBFS-to-dBm measurements across this band. Empty or absent
   * means uncalibrated, and the meter has to say so rather than quote S-units
   * it cannot stand behind.
   */
  calibration?: CalibrationPoint[];
}

export interface SiteDescription {
  name: string;
  operator: string;
  location: string;
  grid: string;
  /** The band plan the operator chose ([site] band_plan); absent from older receivers. */
  band_plan?: string;
  antenna: string;
  contact: string;
  website: string;
  notice: string;
  /** AGPL section 13: where the source for this receiver lives. */
  source_url?: string;
  /** False when the operator has turned the chat off. */
  chat?: boolean;
}

export interface ChatMessage {
  type: 'chat';
  id: number;
  name: string;
  text: string;
  at: number;
}

export interface ChatHistoryMessage {
  type: 'chat-history';
  messages: { id: number; name: string; text: string; at: number }[];
}

export interface ChatRefusedMessage {
  type: 'chat-refused';
  reason: string;
}

/** Station details changed from the admin panel, live. */
export interface StationMessage {
  type: 'station';
  site: SiteDescription;
  /** The public decoders now; absent from receivers older than decoders. */
  decoders?: DecoderDescription[];
}

/** The operator's appearance settings; see state/theme.ts for what is done with them. */
export interface ThemeMessage {
  type: 'theme';
  theme: unknown;
}

export interface WelcomeMessage {
  type: 'welcome';
  protocol: number;
  capabilities?: string[];
  session: number;
  site: SiteDescription;
  bands: BandDescription[];
  modes: string[];
  limits: {
    max_waterfall_width: number;
    max_waterfall_fps: number;
    min_audio_bitrate: number;
    max_audio_bitrate: number;
    max_users: number;
  };
  /** Present so a page is themed before its first frame, not after it. */
  theme?: unknown;
  /** The decoders the operator made public; absent when there are none. */
  decoders?: DecoderDescription[];
}

export interface DecoderChannel {
  /** `<band>-<mode>-<dial in kHz>`, as decodes name it. */
  id: string;
  band: string;
  mode: string;
  dial: number;
  /** The audio it covers, in Hz above the dial. */
  low: number;
  high: number;
}

export interface DecoderDescription {
  id: string;
  channels: DecoderChannel[];
}

export interface StateMessage {
  ack?: { tune: number; viewport: number; dsp: number };
  type: 'state';
  band: string;
  freq: number;
  mode: string;
  low: number;
  high: number;
  cw_pitch: number;
  agc: string;
  /** What `agc` comes to in this mode: Auto's choice, or off for NFM. Absent from older servers. */
  agc_effective?: string;
  gain: number;
  nr: number;
  autonotch: boolean;
  /** Audio below this is cut after demodulation; 0 is off. Absent from older servers. */
  highpass?: number;
  /** NFM de-emphasis in microseconds; 0 is flat. Absent from older servers. */
  deemphasis?: number;
  /** Broadcast FM's de-emphasis, microseconds. */
  wfm_deemphasis?: number;
  ctcss_filter?: boolean;
  ctcss_squelch?: number;
  volume: number;
  squelch: number;
  auto_squelch?: boolean;
  audio_enabled: boolean;
  audio_bitrate: number;
  audio_rate?: number;
  audio_codec?: 'nac' | 'nac2' | 'nac3';
  audio_frames?: number;
  noise_margin?: number;
  filter_limit?: number;
  notches: { hz: number; width: number }[];
  viewport: { enabled: boolean; low: number; high: number; width: number; fps: number };
  note?: string;
}

export interface MeterMessage {
  type: 'meter';
  dbfs: number;
  gain_db: number;
  squelch_open: boolean;
  /** How far the passband is from looking like noise. See AutoSquelch. */
  squelch_statistic?: number;
  pll_locked?: boolean;
  pll_offset?: number;
  /** The CTCSS tone in NFM, Hz; 0 for none. Absent in other modes. */
  ctcss?: number;
  audio_bps: number;
  waterfall_bps: number;
  waterfall_fps: number;
  listeners: number;
}

/**
 * What a broadcast FM station sends by RDS, as far as it has been received;
 * sent when it changes, at most once a second, and empty when there is none
 * (another station, another mode). `pi` is four hex digits.
 */
export interface RdsMessage {
  type: 'rds';
  pi?: string;
  pty?: number;
  tp?: boolean;
  ps?: string;
  rt?: string;
}

export interface AudioConfigMessage {
  type: 'audio-config';
  generation: number;
  rate: number;
  frame_samples: number;
  bitrate: number;
}

export interface ErrorMessage {
  type: 'error';
  message: string;
}

export interface PongMessage {
  type: 'pong';
  t: number;
}

/** The receiver will let this listener's place go in `seconds` without activity. */
export interface InactivityMessage {
  type: 'inactivity';
  seconds: number;
}

export interface BandStatusMessage {
  type: 'band-status';
  bands: Pick<BandDescription, 'id' | 'listeners' | 'running' | 'history' | 'on_air' | 'next_change'>[];
}

export type ServerMessage =
  | WelcomeMessage
  | BandStatusMessage
  | ThemeMessage
  | StationMessage
  | ChatMessage
  | ChatHistoryMessage
  | ChatRefusedMessage
  | StateMessage
  | MeterMessage
  | RdsMessage
  | AudioConfigMessage
  | ErrorMessage
  | InactivityMessage
  | PongMessage;
