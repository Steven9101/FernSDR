/**
 * Recording what the listener hears, in the browser: the received audio,
 * encoded by the browser's own recorder (Opus in WebM or Ogg, AAC in MP4 on
 * Safari), about 11 MB an hour. Nothing is uploaded; the file is offered to
 * the listener when the recording stops.
 */
export interface RecordingSource {
  recordingStream(): { stream: MediaStream; release: () => void } | null;
}

/** An hour; past it the recording stops by itself and is kept. */
export const MAX_RECORDING_MS = 60 * 60_000;

const TYPES = ['audio/webm;codecs=opus', 'audio/ogg;codecs=opus', 'audio/mp4', 'audio/webm'];

export function recordingType(): string | null {
  if (typeof MediaRecorder === 'undefined') return null;
  return TYPES.find((type) => MediaRecorder.isTypeSupported(type)) ?? null;
}

export function extensionFor(type: string): string {
  return type.startsWith('audio/mp4') ? 'm4a' : type.startsWith('audio/ogg') ? 'ogg' : 'webm';
}

/** fernsdr-7074.000kHz-usb-20260928T101500Z.webm */
export function recordingName(freqHz: number, mode: string, startedMs: number, extension: string): string {
  const stamp = new Date(startedMs).toISOString().replace(/[-:]/g, '').replace(/\.\d+Z$/, 'Z');
  return `fernsdr-${(freqHz / 1000).toFixed(3)}kHz-${mode}-${stamp}.${extension}`;
}

export class Recording {
  readonly started = Date.now();
  readonly type: string;
  private readonly recorder: MediaRecorder;
  private readonly chunks: Blob[] = [];
  private readonly release: () => void;
  private readonly timer: ReturnType<typeof setTimeout>;
  private finished: Promise<Blob>;

  private constructor(stream: MediaStream, release: () => void, type: string) {
    this.type = type;
    this.release = release;
    this.recorder = new MediaRecorder(stream, { mimeType: type, audioBitsPerSecond: 24_000 });
    this.finished = new Promise((resolve) => {
      this.recorder.ondataavailable = (event) => {
        if (event.data.size > 0) this.chunks.push(event.data);
      };
      this.recorder.onstop = () => {
        this.release();
        resolve(new Blob(this.chunks, { type: this.type }));
      };
    });
    // Chunks every few seconds, so a crash of the tab loses little and the
    // browser never holds one enormous buffer.
    this.recorder.start(5000);
    this.timer = setTimeout(() => this.stop(), MAX_RECORDING_MS);
  }

  /** Starts recording, or says why it cannot. */
  static start(source: RecordingSource): Recording | string {
    const type = recordingType();
    if (!type) return 'This browser cannot record audio.';
    const tap = source.recordingStream();
    if (!tap) return 'Start the audio first; there is nothing to record yet.';
    return new Recording(tap.stream, tap.release, type);
  }

  /** Stops, and gives the recording. Calling it again gives the same one. */
  stop(): Promise<Blob> {
    clearTimeout(this.timer);
    if (this.recorder.state !== 'inactive') this.recorder.stop();
    return this.finished;
  }
}
