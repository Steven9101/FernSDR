/**
 * The audio thread: decodes NAC frames and feeds the output device.
 *
 * Decoding happens here rather than on the main thread so that a busy UI - a
 * waterfall redraw, a layout pass - can never delay audio. Everything after
 * construction is allocation-free for the same reason.
 *
 * The work itself is in `NacPlaybackEngine`, because the same engine also has
 * to run without a worklet: `audioWorklet` exists only in a secure context, and
 * a receiver served over plain HTTP has none. This file is the worklet host and
 * nothing else.
 */
import { NacPlaybackEngine, type PlaybackMessage } from './playback-engine';

class NacPlaybackProcessor extends AudioWorkletProcessor {
  private engine = new NacPlaybackEngine(sampleRate, (stats) =>
    this.port.postMessage({ type: 'stats', ...stats }),
  );

  constructor() {
    super();
    this.port.onmessage = (event: MessageEvent<PlaybackMessage>) => this.engine.handle(event.data);
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const output = outputs[0];
    if (!output || output.length === 0) return true;
    const channel = output[0];
    this.engine.render(channel);
    for (let c = 1; c < output.length; c++) output[c].set(channel);
    return true;
  }
}

registerProcessor('nac-playback', NacPlaybackProcessor);
