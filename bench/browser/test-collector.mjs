import assert from 'node:assert/strict';
import test from 'node:test';
import {TapCollector, CounterDecoder} from './lib.mjs';

test('measurement window excludes startup and preserves exact context frames', () => {
  const c = {rate: 12000, blocks: [{frame: 10, data: new Float32Array([1,2,3,4])},
                                 {frame: 14, data: new Float32Array([5,6,7,8])}]};
  const r = TapCollector.assemble(c, {first: 12, end: 16});
  assert.equal(r.first, 12);
  assert.deepEqual([...r.x], [3,4,5,6]);
  assert.equal(r.holes, 0);
});

test('a missing block remains a hole rather than invented silence', () => {
  const c = {rate: 12000, blocks: [{frame: 10, data: new Float32Array([1,2])},
                                 {frame: 14, data: new Float32Array([5,6])}]};
  const r = TapCollector.assemble(c);
  assert.equal(r.holes, 2);
  assert.ok(Number.isNaN(r.x[2]));
});

test('frame jump cannot request an unbounded assembly allocation', () => {
  const c = {rate: 12000, blocks: [{frame: 0, data: new Float32Array([1])},
                                 {frame: 1e12, data: new Float32Array([1])}]};
  assert.throws(() => TapCollector.assemble(c), /span exceeds/);
});

test('collection budget fails closed and stops retaining events', () => {
  const tap = new TapCollector();
  tap.events = 150000;
  tap.on('context', {id: 'new', sampleRate: 48000});
  assert.equal(tap.overflow, true);
  assert.equal(tap.contexts.size, 0);
  assert.equal(tap.errors.length, 1);
  tap.on('context', {id: 'another', sampleRate: 48000});
  assert.equal(tap.errors.length, 1);
});

test('distinct document/context keys retain both sides of a navigation', () => {
  const tap = new TapCollector();
  tap.on('context', {id: 'document-a-0', sampleRate: 48000});
  tap.on('context', {id: 'document-b-0', sampleRate: 48000});
  assert.equal(tap.contexts.size, 2);
});

function counterBytes(ticks) {
  const bytes = Buffer.alloc(ticks.length*8);
  ticks.forEach((tick, i) => {
    bytes.writeFloatLE(i+.25, i*8);
    bytes.writeFloatLE((tick+1)/(1<<20), i*8+4);
  });
  return bytes;
}

test('PCM counter preserves time across chunk boundaries and counter rollover', () => {
  const blocks=[], errors=[];
  const decoder=new CounterDecoder({start:512, period:1<<20, rate:48000},
    (frame,data)=>blocks.push({frame,data:[...data]}), e=>errors.push(e));
  decoder.push(counterBytes([(1<<20)-2, (1<<20)-1]));
  decoder.push(counterBytes([0,1]));
  assert.deepEqual(errors,[]);
  assert.equal(blocks[0].frame,512+(1<<20)-2);
  assert.equal(blocks[1].frame,512+(1<<20));
});

test('missing recorder quantum remains a 128-frame gap', () => {
  const blocks=[], errors=[];
  const decoder=new CounterDecoder({start:512, period:1<<20, rate:48000},
    (frame,data)=>blocks.push({frame,data:[...data]}), e=>errors.push(e));
  decoder.push(counterBytes([0,1,130,131]));
  assert.deepEqual(blocks,[{frame:512,data:[.25,1.25]},{frame:642,data:[2.25,3.25]}]);
  assert.deepEqual(errors,['PCM capture lost 128 frames']);
});
