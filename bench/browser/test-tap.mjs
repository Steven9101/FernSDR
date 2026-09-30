import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';

async function graph(ready = true) {
  const edges = new Map();
  class AudioNode {
    constructor(context) { this.context = context; }
    connect(destination, output = 0, input = 0) {
      const list = edges.get(this) ?? [];
      if (!list.some(e => e.destination === destination && e.output === output && e.input === input))
        list.push({destination, output, input});
      edges.set(this, list);
      return destination;
    }
    disconnect(destination, output, input) {
      const match = e => arguments.length === 0 ||
        (typeof destination === 'number' ? e.output === destination :
          e.destination === destination && (output === undefined || e.output === output) &&
          (input === undefined || e.input === input));
      edges.set(this, (edges.get(this) ?? []).filter(e => !match(e)));
    }
  }
  class AudioDestinationNode extends AudioNode {}
  let release;
  const loaded = new Promise(resolve => { release = resolve; });
  class AudioContext {
    constructor() {
      this.destination = new AudioDestinationNode(this);
      this.audioWorklet = {addModule: () => loaded};
      this.sampleRate = 48000;
    }
  }
  class AudioWorkletNode extends AudioNode { constructor(ctx) { super(ctx); this.port = {}; } }
  const window = {AudioContext, __fbTapSend() {}};
  vm.runInNewContext(fs.readFileSync(new URL('tap.js', import.meta.url), 'utf8'), {
    window, AudioNode, AudioDestinationNode, AudioWorkletNode, GainNode: AudioNode,
    URL: {createObjectURL: () => 'blob:mock'}, Blob, WeakMap, WeakSet,
    document: {addEventListener() {}}, performance: {timeOrigin: 1000}, setInterval() {},
  });
  const ctx = new window.AudioContext();
  if (ready) { release(); await loaded; await Promise.resolve(); }
  return {ctx, edges, node: () => new AudioNode(ctx), release, loaded, entry: window.__fbTap.contexts[0]};
}

test('disconnecting audible destination removes recorder edge, reconnect restores it', async () => {
  const g = await graph(); const src = g.node();
  src.connect(g.ctx.destination);
  assert.equal(g.edges.get(src).length, 2);
  src.disconnect(g.ctx.destination);
  assert.equal(g.edges.get(src).length, 0);
  src.connect(g.ctx.destination);
  assert.equal(g.edges.get(src).length, 2);
});

test('disconnect before worklet loads cannot leave a queued ghost source', async () => {
  const g = await graph(false); const src = g.node();
  src.connect(g.ctx.destination); src.disconnect();
  g.release(); await g.loaded; await Promise.resolve();
  assert.equal(g.edges.get(src).length, 0);
});

test('tap mirrors selected outputs and preserves unrelated connections', async () => {
  const g = await graph(); const src = g.node(); const other = g.node();
  src.connect(g.ctx.destination, 1); src.connect(other, 0);
  assert.ok(g.edges.get(src).some(e => e.destination === g.entry.tap && e.output === 1));
  src.disconnect(1);
  assert.deepEqual(g.edges.get(src).map(e => e.destination), [other]);
});

test('recorder silent pull connection is never tapped back into itself', async () => {
  const g = await graph();
  const mute = g.edges.get(g.entry.tap)[0].destination;
  assert.equal(g.edges.get(mute).length, 1);
  assert.equal(g.edges.get(mute)[0].destination, g.ctx.destination);
});
