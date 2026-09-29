import { afterEach, describe, expect, it, vi } from 'vitest';
import { AudioPlayer } from './player';
import { NacPlaybackEngine } from './playback-engine';

afterEach(() => { vi.restoreAllMocks(); vi.unstubAllGlobals(); });

function audioEnvironment(worklet: boolean, failFirst = false) {
  const contexts: FakeContext[] = [];
  const posts = vi.fn();
  let nodes = 0;
  class FakeContext {
    sampleRate = 48000;
    currentTime = 0;
    state = 'suspended';
    destination = {};
    onstatechange: (() => void) | null = null;
    audioWorklet = worklet ? { addModule: vi.fn(async () => {
      if (failFirst && contexts.length === 1) throw new Error('module unavailable');
    }) } : undefined;
    constructor() { contexts.push(this); }
    createGain() { return { connect() {}, disconnect() {}, gain: { setTargetAtTime() {} } }; }
    createBiquadFilter() { return { connect() {}, disconnect() {}, type: '', frequency: { value: 0 }, gain: { setTargetAtTime() {} } }; }
    createScriptProcessor() { return { connect() {}, disconnect() {}, onaudioprocess: null }; }
    async resume() { this.state = 'running'; this.onstatechange?.(); }
    async close() { this.state = 'closed'; }
  }
  vi.stubGlobal('AudioContext', FakeContext);
  vi.stubGlobal('AudioWorkletNode', class {
    port = { postMessage: posts, onmessage: null };
    constructor() { nodes++; }
    connect() {}
    disconnect() {}
  });
  return { contexts, posts, nodes: () => nodes };
}

describe('audio output lifecycle', () => {
  for (const worklet of [false, true]) it(`carries the stream flags through ${worklet ? 'worklet' : 'fallback'} playback`, async () => {
    const env = audioEnvironment(worklet);
    const handle = vi.spyOn(NacPlaybackEngine.prototype, 'handle');
    const player = new AudioPlayer();
    player.configure(12000, 1);
    await player.start();
    player.feed({ sequence: 5, generation: 1, muted: false, compact: true, discontinuity: true, payload: Uint8Array.from([2, 0]) });
    const packet = expect.objectContaining({ type: 'packet', compact: true, discontinuity: true, sequence: 5, generation: 1 });
    if (worklet) expect(env.posts).toHaveBeenLastCalledWith(packet, expect.any(Array));
    else expect(handle).toHaveBeenLastCalledWith(packet);
  });
  it('applies format changes to the HTTP fallback after startup', async () => {
    audioEnvironment(false);
    const handle = vi.spyOn(NacPlaybackEngine.prototype, 'handle');
    const player = new AudioPlayer();
    player.configure(12000, 1);
    await player.start();
    expect(player.usingFallback).toBe(true);
    player.configure(16000, 2);
    expect(handle).toHaveBeenLastCalledWith({ type: 'config', rate: 16000, generation: 2 });
  });

  it('shares output setup between repeated taps', async () => {
    const env = audioEnvironment(true);
    const player = new AudioPlayer();
    player.configure(12000, 1);
    await Promise.all([player.start(), player.start(), player.start()]);
    expect(env.contexts).toHaveLength(1);
    expect(env.nodes()).toBe(1);
    expect(player.state).toBe('running');
    expect(env.posts).toHaveBeenCalledWith({ type: 'config', rate: 12000, generation: 1 });
  });

  it('rebuilds the output after a failed module load and keeps its format', async () => {
    const env = audioEnvironment(true, true);
    const player = new AudioPlayer();
    player.configure(16000, 3);
    await player.start();
    expect(player.state).toBe('failed');
    expect(env.contexts[0].state).toBe('closed');
    await player.start();
    expect(env.contexts).toHaveLength(2);
    expect(player.state).toBe('running');
    expect(env.posts).toHaveBeenCalledWith({ type: 'config', rate: 16000, generation: 3 });
  });
});
