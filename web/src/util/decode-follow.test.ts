import { describe, expect, it } from 'vitest';
import type { DecoderChannel } from '../net/protocol';
import { channelAt, DecodesFollower } from './decode-follow';

const ft8: DecoderChannel = { id: '20m-ft8-14074', band: '20m', mode: 'ft8', dial: 14074000, low: 0, high: 3000 };
const ft4: DecoderChannel = { id: '20m-ft4-14080', band: '20m', mode: 'ft4', dial: 14080000, low: 0, high: 3000 };

describe('following decodes', () => {
  it('finds the channel from a kHz below the dial to the top of its audio', () => {
    expect(channelAt([ft8, ft4], '20m', 14074000)?.id).toBe('20m-ft8-14074');
    expect(channelAt([ft8, ft4], '20m', 14073000)?.id).toBe('20m-ft8-14074');
    expect(channelAt([ft8, ft4], '20m', 14077000)?.id).toBe('20m-ft8-14074');
    expect(channelAt([ft8, ft4], '20m', 14081500)?.id).toBe('20m-ft4-14080');
    expect(channelAt([ft8, ft4], '20m', 14072900)).toBeNull();
    expect(channelAt([ft8, ft4], '40m', 14074000)).toBeNull();
  });

  it('opens the decodes and puts the listener back where they were', () => {
    const follow = new DecodesFollower();
    expect(follow.settled('20m-ft8-14074', 'display', true)).toBe('decodes');
    // Moving to another channel keeps the decodes open.
    expect(follow.settled('20m-ft4-14080', 'decodes', true)).toBeNull();
    expect(follow.settled(null, 'decodes', true)).toBe('display');
  });

  it('never takes away a tab the listener chose', () => {
    const follow = new DecodesFollower();
    expect(follow.settled('20m-ft8-14074', 'receive', true)).toBe('decodes');
    follow.chose();
    expect(follow.settled(null, 'decodes', true)).toBeNull();
    // Closed while on the channel: not pushed again until they come back.
    const again = new DecodesFollower();
    expect(again.settled('20m-ft8-14074', 'receive', true)).toBe('decodes');
    again.chose();
    expect(again.settled('20m-ft8-14074', 'receive', true)).toBeNull();
    expect(again.settled(null, 'receive', true)).toBeNull();
    expect(again.settled('20m-ft8-14074', 'receive', true)).toBe('decodes');
  });

  it('does nothing where the decodes are not public or already open', () => {
    const follow = new DecodesFollower();
    expect(follow.settled('20m-ft8-14074', 'receive', false)).toBeNull();
    expect(follow.settled(null, 'receive', false)).toBeNull();
    const open = new DecodesFollower();
    expect(open.settled('20m-ft8-14074', 'decodes', true)).toBeNull();
    expect(open.settled(null, 'decodes', true)).toBeNull();
  });
});
