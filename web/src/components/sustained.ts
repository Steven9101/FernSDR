/**
 * A condition shown only once it has held for a while, and hidden only once
 * it has been gone for a while. The waterfall rate the server reports moves
 * a line or so a second either way as it fits the stream into its budget; a
 * notice that followed it directly appeared and vanished every second and
 * moved the panel with it.
 */
export class Sustained {
  private since = -1;
  private clearSince = -1;
  shown = false;

  constructor(private readonly onAfterMs: number, private readonly offAfterMs: number) {}

  update(now: number, active: boolean): boolean {
    if (active) {
      this.clearSince = -1;
      if (this.since < 0) this.since = now;
      if (!this.shown && now - this.since >= this.onAfterMs) this.shown = true;
    } else {
      this.since = -1;
      if (this.clearSince < 0) this.clearSince = now;
      if (this.shown && now - this.clearSince >= this.offAfterMs) this.shown = false;
    }
    return this.shown;
  }
}
