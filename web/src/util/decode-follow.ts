import type { DecoderChannel } from '../net/protocol';

/**
 * The decoder channel a listener tuned to `freq` on `band` is on: the dial up
 * to the top of the channel's audio, as USB lays it out, and a kHz below the
 * dial, where a click on the band plan's FT8 mark or a typed 14.074 lands.
 */
export function channelAt(channels: readonly DecoderChannel[], band: string, freq: number): DecoderChannel | null {
  for (const channel of channels) {
    if (channel.band === band && freq >= channel.dial - 1000 && freq <= channel.dial + channel.high) return channel;
  }
  return null;
}

/**
 * Opens the Decodes tab when a listener settles on a decoded frequency and
 * puts back the tab they had when they leave it, as long as the tab was
 * opened for them and they have not picked one since: a tab the listener
 * chose is never taken away, and one they closed is not pushed on them again
 * until they leave the channel and come back.
 */
export class DecodesFollower {
  private channel: string | null = null;
  private opened = false;
  private before = 'receive';

  /** The listener settled on `channel` (null for none); the tab to show now, or null to leave it. */
  settled(channel: string | null, tab: string, available: boolean): string | null {
    if (channel === this.channel) return null;
    const leaving = this.channel !== null && channel === null;
    this.channel = channel;
    if (leaving) {
      const back = this.opened && tab === 'decodes' ? this.before : null;
      this.opened = false;
      return back;
    }
    if (channel === null || !available || tab === 'decodes') return null;
    this.before = tab;
    this.opened = true;
    return 'decodes';
  }

  /** The listener picked a tab themselves. */
  chose(): void {
    this.opened = false;
  }
}
