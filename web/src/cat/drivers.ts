/**
 * What it takes to talk to a radio over its CAT port: the commands that ask
 * for the dial and the mode, the ones that set them, and a parser for the
 * answers. Pure byte handling, no serial port, so every driver is tested
 * byte for byte without a radio.
 *
 * The frequency exchanged is the one the radio's dial shows, which for every
 * mode here is also where this receiver puts the signal (for CW the radio
 * shows the carrier the listener hears at the pitch), so no offsets are
 * applied in either direction.
 */

/** The receiver's modes, the only ones a radio's mode is translated to. */
export type ReceiverMode = 'usb' | 'lsb' | 'cw' | 'cwl' | 'am' | 'sam' | 'nfm' | 'dsb';

export interface RigReport {
  freq?: number;
  mode?: ReceiverMode;
}

export interface RigParser {
  /** Bytes as they arrive, in any chunking; returns what they completed. */
  feed(bytes: Uint8Array): RigReport[];
}

export interface RigDriver {
  id: DriverId;
  label: string;
  /** What the radios this covers usually ship with. */
  defaultBaud: number;
  /** The dial's step in Hz: differences smaller than this are the same frequency. */
  resolutionHz: number;
  /** Whether the driver needs a CI-V address. */
  needsAddress: boolean;
  /** Stop bits the radio's port uses. */
  stopBits: 1 | 2;
  /**
   * The least time between two commands, ms. The FT-817 family takes a
   * command as five bytes arriving within 200 ms and drops a partial one after
   * that; commands closer together than that let one lost byte shift every
   * later command, and a shifted frequency byte can land in the opcode's
   * place: 0x08 is PTT on.
   */
  commandGapMs: number;
  create(address: number): RigSession;
}

export interface RigSession {
  /** Asked five times a second. */
  poll(): Uint8Array[];
  setFrequency(hz: number): Uint8Array;
  setMode(mode: ReceiverMode): Uint8Array | null;
  parser: RigParser;
  /**
   * For protocols whose answers carry no framing: called before each poll, so
   * a stray byte cannot shift every answer after it.
   */
  beforePoll?(): void;
}

export type DriverId = 'kenwood' | 'yaesu' | 'yaesu-classic' | 'icom';

const ascii = (text: string) => new TextEncoder().encode(text);

// --- Kenwood, Elecraft, FlexRadio, and Yaesu's newer radios --------------
//
// Both speak "FA" for the dial and "MD" for the mode, terminated by ';'. The
// digit count of FA differs by model (11 on Kenwood and Elecraft, 9 on the
// FT-991 and FTDX10, 8 on the FT-950), and a set with the wrong count is
// refused, so the count is learned from the radio's first answer.

type TextFlavour = {
  modeQuery: string;
  modePrefix: string;
  modes: Record<string, ReceiverMode>;
  toRig: Record<ReceiverMode, string>;
  digits: number;
};

const KENWOOD: TextFlavour = {
  modeQuery: 'MD;',
  modePrefix: 'MD',
  // 6 and 9 are FSK/DATA; heard as sideband.
  modes: { '1': 'lsb', '2': 'usb', '3': 'cw', '4': 'nfm', '5': 'am', '6': 'lsb', '7': 'cwl', '9': 'usb' },
  toRig: { lsb: '1', usb: '2', cw: '3', cwl: '7', nfm: '4', am: '5', sam: '5', dsb: '5' },
  digits: 11,
};

const YAESU: TextFlavour = {
  modeQuery: 'MD0;',
  modePrefix: 'MD0',
  modes: {
    '1': 'lsb', '2': 'usb', '3': 'cw', '4': 'nfm', '5': 'am', '6': 'lsb', '7': 'cwl', '8': 'lsb',
    '9': 'usb', A: 'nfm', B: 'nfm', C: 'usb', D: 'am', E: 'usb', F: 'nfm',
  },
  toRig: { lsb: '1', usb: '2', cw: '3', cwl: '7', nfm: '4', am: '5', sam: '5', dsb: '5' },
  digits: 9,
};

function textSession(flavour: TextFlavour): RigSession {
  let digits = flavour.digits;
  let pending = '';
  return {
    poll: () => [ascii('FA;'), ascii(flavour.modeQuery)],
    setFrequency: (hz) => ascii(`FA${String(Math.round(hz)).padStart(digits, '0').slice(-digits)};`),
    setMode: (mode) => (mode in flavour.toRig ? ascii(`${flavour.modePrefix}${flavour.toRig[mode]};`) : null),
    parser: {
      feed(bytes) {
        pending += new TextDecoder().decode(bytes);
        // A radio that is not answering CAT sends nothing; one on the wrong
        // baud rate sends noise: never let either grow without bound.
        if (pending.length > 256) pending = pending.slice(-64);
        const reports: RigReport[] = [];
        let end: number;
        while ((end = pending.indexOf(';')) >= 0) {
          const answer = pending.slice(0, end).trim();
          pending = pending.slice(end + 1);
          const freq = /^FA(\d{6,12})$/.exec(answer);
          if (freq) {
            digits = freq[1].length;
            reports.push({ freq: Number(freq[1]) });
            continue;
          }
          if (answer.startsWith(flavour.modePrefix)) {
            const mode = flavour.modes[answer.slice(flavour.modePrefix.length).toUpperCase()];
            if (mode) reports.push({ mode });
          }
          // Anything else ('?', IF answers, auto-information we did not ask
          // for) is not about the dial or the mode.
        }
        return reports;
      },
    },
  };
}

// --- Yaesu FT-817, FT-818, FT-857, FT-897 -----------------------------------
//
// Five-byte binary commands, the opcode last. "Read frequency and mode" (03)
// answers with five bytes: the frequency in tens of hertz as eight BCD
// digits, most significant first, then the mode.

const CLASSIC_MODES: Record<number, ReceiverMode> = {
  0x00: 'lsb', 0x01: 'usb', 0x02: 'cw', 0x03: 'cwl', 0x04: 'am', 0x06: 'nfm', 0x08: 'nfm', 0x0a: 'usb', 0x0c: 'nfm',
  // The FT-857 and FT-897 set the top bit for their narrow filters.
  0x82: 'cw', 0x83: 'cwl', 0x88: 'nfm',
};
const CLASSIC_TO_RIG: Record<ReceiverMode, number> = { lsb: 0x00, usb: 0x01, cw: 0x02, cwl: 0x03, am: 0x04, sam: 0x04, dsb: 0x04, nfm: 0x08 };

function bcdBigEndian(value: number, bytes: number): number[] {
  const digits = String(Math.round(value)).padStart(bytes * 2, '0').slice(-bytes * 2);
  return Array.from({ length: bytes }, (_, i) => (Number(digits[2 * i]) << 4) | Number(digits[2 * i + 1]));
}

function fromBcd(byte: number): number {
  return (byte >> 4) * 10 + (byte & 0x0f);
}

function classicSession(): RigSession {
  let pending: number[] = [];
  return {
    poll: () => [new Uint8Array([0, 0, 0, 0, 0x03])],
    setFrequency: (hz) => new Uint8Array([...bcdBigEndian(hz / 10, 4), 0x01]),
    setMode: (mode) => (mode in CLASSIC_TO_RIG ? new Uint8Array([CLASSIC_TO_RIG[mode], 0, 0, 0, 0x07]) : null),
    beforePoll() {
      pending = [];
    },
    parser: {
      feed(bytes) {
        pending.push(...bytes);
        const reports: RigReport[] = [];
        while (pending.length >= 5) {
          const answer = pending.splice(0, 5);
          if (answer.slice(0, 4).some((b) => (b >> 4) > 9 || (b & 0x0f) > 9)) continue;
          const tens = answer.slice(0, 4).reduce((sum, b) => sum * 100 + fromBcd(b), 0);
          const mode = CLASSIC_MODES[answer[4]];
          reports.push(mode ? { freq: tens * 10, mode } : { freq: tens * 10 });
        }
        return reports;
      },
    },
  };
}

// --- Icom CI-V ----------------------------------------------------------------
//
// Frames FE FE <to> <from> <command> <data> FD. The frequency is five BCD
// bytes, least significant first. The CI-V line is one wire shared by
// everyone on it, so our own frames come back to us: those are dropped by
// their sender address.

export const CONTROLLER_ADDRESS = 0xe0;

const ICOM_MODES: Record<number, ReceiverMode> = {
  0x00: 'lsb', 0x01: 'usb', 0x02: 'am', 0x03: 'cw', 0x04: 'lsb', 0x05: 'nfm', 0x06: 'nfm', 0x07: 'cwl', 0x08: 'usb',
};
const ICOM_TO_RIG: Record<ReceiverMode, number> = { lsb: 0x00, usb: 0x01, am: 0x02, sam: 0x02, dsb: 0x02, cw: 0x03, cwl: 0x07, nfm: 0x05 };

function icomSession(address: number): RigSession {
  const frame = (command: number, data: number[] = []) =>
    new Uint8Array([0xfe, 0xfe, address, CONTROLLER_ADDRESS, command, ...data, 0xfd]);
  let pending: number[] = [];
  return {
    poll: () => [frame(0x03), frame(0x04)],
    setFrequency: (hz) => frame(0x05, bcdBigEndian(hz, 5).reverse()),
    setMode: (mode) => (mode in ICOM_TO_RIG ? frame(0x06, [ICOM_TO_RIG[mode]]) : null),
    parser: {
      feed(bytes) {
        pending.push(...bytes);
        if (pending.length > 512) pending = pending.slice(-64);
        const reports: RigReport[] = [];
        for (;;) {
          const start = pending.findIndex((b, i) => b === 0xfe && pending[i + 1] === 0xfe);
          if (start < 0) {
            pending = pending.slice(-1);
            break;
          }
          const end = pending.indexOf(0xfd, start + 2);
          if (end < 0) {
            pending = pending.slice(start);
            break;
          }
          const body = pending.slice(start + 2, end);
          pending = pending.slice(end + 1);
          const [to, from, command, ...data] = body;
          if (from !== address) continue;
          if (to !== CONTROLLER_ADDRESS && to !== 0x00) continue;
          if ((command === 0x03 || command === 0x00) && (data.length === 5 || data.length === 4)) {
            if (data.some((b) => (b >> 4) > 9 || (b & 0x0f) > 9)) continue;
            const freq = data.reduceRight((sum, b) => sum * 100 + fromBcd(b), 0);
            reports.push({ freq });
          } else if ((command === 0x04 || command === 0x01) && data.length >= 1) {
            const mode = ICOM_MODES[data[0]];
            if (mode) reports.push({ mode });
          }
        }
        return reports;
      },
    },
  };
}

export const DRIVERS: readonly RigDriver[] = [
  { id: 'kenwood', label: 'Kenwood, Elecraft, FlexRadio', defaultBaud: 38400, resolutionHz: 1, needsAddress: false, stopBits: 1, commandGapMs: 0, create: () => textSession(KENWOOD) },
  { id: 'yaesu', label: 'Yaesu (FT-991, FT-710, FTDX10, FTDX101 and similar)', defaultBaud: 38400, resolutionHz: 1, needsAddress: false, stopBits: 1, commandGapMs: 0, create: () => textSession(YAESU) },
  // 4800 baud and two stop bits are what these radios ship with.
  { id: 'yaesu-classic', label: 'Yaesu FT-817, FT-818, FT-857, FT-897', defaultBaud: 4800, resolutionHz: 10, needsAddress: false, stopBits: 2, commandGapMs: 250, create: () => classicSession() },
  { id: 'icom', label: 'Icom (CI-V)', defaultBaud: 19200, resolutionHz: 1, needsAddress: true, stopBits: 1, commandGapMs: 0, create: (address) => icomSession(address) },
];

/** CI-V addresses the radios ship with; the operator can change them in the radio's menu. */
export const ICOM_ADDRESSES: readonly { label: string; address: number }[] = [
  { label: 'IC-7300', address: 0x94 },
  { label: 'IC-705', address: 0xa4 },
  { label: 'IC-9700', address: 0xa2 },
  { label: 'IC-7610', address: 0x98 },
  { label: 'IC-7100', address: 0x88 },
  { label: 'IC-7851', address: 0x8e },
];
