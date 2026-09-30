import { afterEach, describe, expect, it, vi } from 'vitest';
import { extensionFor, MAX_RECORDING_MS, Recording, recordingName } from './recorder';

class FakeRecorder {
  static isTypeSupported = (type: string) => type === 'audio/webm;codecs=opus';
  state = 'inactive';
  ondataavailable: ((event: { data: Blob }) => void) | null = null;
  onstop: (() => void) | null = null;
  constructor(public stream: unknown, public options: { mimeType: string }) {}
  start() { this.state = 'recording'; }
  stop() {
    this.state = 'inactive';
    this.ondataavailable?.({ data: new Blob(['abc']) });
    this.onstop?.();
  }
}

afterEach(() => { vi.unstubAllGlobals(); vi.useRealTimers(); });

describe('recorder', () => {
  it('names the file after the frequency, the mode and the UTC start', () => {
    expect(recordingName(7_074_000, 'usb', Date.UTC(2026, 8, 28, 10, 15, 0), 'webm')).toBe('fernsdr-7074.000kHz-usb-20260928T101500Z.webm');
    expect(extensionFor('audio/mp4')).toBe('m4a');
    expect(extensionFor('audio/ogg;codecs=opus')).toBe('ogg');
  });

  it('says why it cannot start, and records and releases the tap when it can', async () => {
    vi.stubGlobal('MediaRecorder', undefined);
    expect(Recording.start({ recordingStream: () => null })).toBe('This browser cannot record audio.');
    vi.stubGlobal('MediaRecorder', FakeRecorder);
    expect(Recording.start({ recordingStream: () => null })).toBe('Start the audio first; there is nothing to record yet.');
    const release = vi.fn();
    const recording = Recording.start({ recordingStream: () => ({ stream: {} as MediaStream, release }) }) as Recording;
    expect(recording.type).toBe('audio/webm;codecs=opus');
    const blob = await recording.stop();
    expect(blob.size).toBe(3);
    expect(release).toHaveBeenCalledTimes(1);
    expect(await recording.stop()).toBe(blob);
  });

  it('stops by itself after an hour', async () => {
    vi.useFakeTimers();
    vi.stubGlobal('MediaRecorder', FakeRecorder);
    const recording = Recording.start({ recordingStream: () => ({ stream: {} as MediaStream, release: () => {} }) }) as Recording;
    vi.advanceTimersByTime(MAX_RECORDING_MS);
    expect((await recording.stop()).size).toBe(3);
  });

  it('tells the page when the hour ends it, so the file is offered without a press', async () => {
    vi.useFakeTimers();
    vi.stubGlobal('MediaRecorder', FakeRecorder);
    const recording = Recording.start({ recordingStream: () => ({ stream: {} as MediaStream, release: () => {} }) }) as Recording;
    let ended: Blob | null = null;
    void recording.ended.then((blob) => (ended = blob));
    await vi.advanceTimersByTimeAsync(MAX_RECORDING_MS - 1);
    expect(ended).toBeNull();
    expect(recording.reachedLimit).toBe(false);
    await vi.advanceTimersByTimeAsync(1);
    expect(ended!.size).toBe(3);
    expect(recording.reachedLimit).toBe(true);
  });
});
