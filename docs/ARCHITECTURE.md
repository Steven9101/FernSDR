# Architecture

In plain words: the radio delivers one fast stream of samples for a whole
band. FernSDR turns it into frequencies once, however many people listen,
and then cuts each listener's own slice out of that, turns it into sound and
sends it, together with the picture of the band. The parts below are that
in detail.

## The shape of the problem

A receiver pays for shared transforms, per-listener DSP and network delivery.
Which one limits capacity depends on the captured bandwidth, listening modes,
hardware and uplink. A 64 Msps source can be limited by the shared work even
with few listeners.

## Doing the expensive work once

The channelizer transforms a band **once per block**, however many people are
listening. Each listener then costs:

- a gather of `L` bins out of the shared spectrum, times their filter mask
- one `L`-point inverse FFT (`L = 256` for a 12 kHz channel)
- one complex rotation per output sample, for sub-bin tuning

Structurally this is overlap-save fast convolution with the decimation folded
into the inverse transform: a forward FFT of size `K` with 50% overlap, and an
inverse of size `L = K/D` where `D` is the decimation factor. The second half
of each inverse block is free of circular wraparound and is the output.

```
                     ┌──────────────── once per band ─────────────────┐
   IQ ──► history ──►│ FFT(K)  ──►  shared spectrum                   │
                     └───────────────────┬───────────────────────────-┘
                                         │
              ┌──────────────────────────┼──────────────────────────┐
              ▼                          ▼                          ▼
     ┌── per listener ───┐      ┌── per listener ───┐      ┌── ... ───┐
     │ gather L bins     │      │ gather L bins     │
     │ × filter mask     │      │ × filter mask     │
     │ IFFT(L)           │      │ IFFT(L)           │
     │ fine rotation     │      │ fine rotation     │
     │ demodulate        │      │ demodulate        │
     │ AGC, NR, notch    │      │ AGC, NR, notch    │
     │ NAC encode        │      │ NAC encode        │
     └───────────────────┘      └───────────────────┘
```

[PERFORMANCE.md](PERFORMANCE.md) has the measurements and [TESTING.md](TESTING.md)
how they are checked.
`tools/loadtest.py` checks delivered audio time as well as CPU and sequence
gaps, because a receiver that never drops a frame can still run too slowly.

### Choosing the transform size

The channelizer's FFT size targets roughly 50 Hz bins. That also puts the
block near 10 ms, because the two constraints turn out to be the same one:
`bin = rate/K` and `block = K/(2*rate)` seconds, so asking for 50 Hz bins gives
`K ≈ rate/50` and therefore a 10 ms block. Fine enough to tune with, short
enough to stay low-latency.

### Sub-bin tuning, and a subtlety worth recording

Tuning splits into an integer bin offset - free, a circular shift of the slice
- and a sub-bin remainder corrected by a rotation on the decimated output,
which is cheap at the low rate. The filter mask is built pre-offset by that
remainder, so the passband edges land exactly where they were asked to rather
than being skewed by up to half a bin.

There is a trap here that cost real debugging time. Selecting bins around
`center_bin` is a down-conversion whose phase reference is the **start of the
FFT window** - and that window restarts every block, while a true
down-conversion's reference runs from absolute time zero. Blocks advance by
`K/2` samples, so the phase owed at block `b` is `-pi * center_bin * b`. Modulo
2π that is either 0 or π, so the correction collapses to a sign flip on odd
blocks when the centre bin is odd. Left out, the channel is tuned exactly one
bin off and smeared - for half of all tuning positions. Unit tests on the
channelizer passed; the end-to-end test caught it.

### A separate transform for the waterfall

Fast convolution requires a rectangular window, whose -13 dB sidelobes would
smear every strong carrier across the waterfall in a visible skirt tens of
kilohertz wide. The spectrum analyser therefore runs its own FFT with a
Blackman-Harris window (sidelobes below -85 dB, measured). It is shared across
every listener on the band. At high sample rates its size and cadence are
material costs, even when each client receives a small row.

## Holding the bandwidth budget

Each listener has a configured ceiling on its combined stream. Backpressure
reduces waterfall traffic first, then audio bitrate if the queue keeps growing.
Recovery is gradual; repeated failed probes wait longer before trying again.
The browser adapts its playout buffer and conceals short gaps. Buffering cannot
provide live audio during an outage, and persistent backlog must be trimmed.

## Threads

Deliberately few:

- **One thread per band.** Reads its source, runs the shared transforms, walks
  its listeners, fills their outboxes.
- **A bounded DSP pool.** Large independent channelizer/waterfall transforms
  can share an idle worker. Groups of at least 64 listeners process in batches
  across the caller and workers. Each band waits for its batch before reading
  the next block; exceptions drain the batch before releasing its buffers.
- **One thread for all I/O.** An epoll loop doing HTTP, WebSocket framing and
  every socket write.

Band threads never touch a socket. They fill a mutex-protected outbox and call
`wake()`; the I/O loop drains it. Because that happens on every wake rather
than on a timer, flush latency tracks the DSP block period rather than a
polling interval.

Settings travel the other way through a second small handoff: the network
thread stores a complete settings struct, and the DSP thread adopts it at a
block boundary. Whole structs, not individual fields - applying a frequency
and a mode separately would leave a listener briefly tuned with the old mode's
passband, which is audible.

## Lifetimes

Sessions queue their output and the transport pulls it. An earlier version
handed each session a callback capturing its `Connection`; when a disconnect
went unnoticed the session outlived the connection and wrote into freed
memory, which in practice delivered several users' audio to one client. The
immediate bug was a missing callback, but the design that allowed it was the
raw reference, and that is what was removed.

## The client

```
 WebSocket ─┬─ JSON control  ──► runes ──► Svelte components
            │
            ├─ audio frames  ──► AudioWorklet: NAC decode, resample, drift
            │                    control, output
            │
            └─ waterfall     ──► line decode ──► WebGL2 ring-buffer texture
```

**Audio decoding uses AudioWorklet** where the browser supports it in the
page's security context. Its buffers are reused. The ScriptProcessor fallback
runs on the main thread and is more exposed to layout or rendering delays.

**Clock drift** is handled by nudging the resampling ratio by a fraction of a
percent. Larger disruptions require a different response: conceal short gaps,
resynchronise after a long outage and trim persistent excess backlog. Those
recovery actions may be audible or interrupt a digital-mode decoder.

**The waterfall stores each line's frequency span per row**, in a second
texture, so the fragment shader can ask every row independently where a given
frequency sat in that row. History stays locked to frequency through pans and
zooms instead of shearing sideways, and a region no line ever covered is drawn
empty rather than smeared with an invented edge pixel.

## What is deliberately absent

- **No digital-mode decoding, SSTV or packet.** The audio path carries them
  intact to software that does this better.
- **No plugin system.** Every feature here is one somebody uses on every
  session.
- **No database, no accounts, no server-side state beyond the live sessions.**
- **No TLS in the server.** A reverse proxy does it better; see
  `docs/DEPLOYMENT.md`.
- **No external dependencies in the server.** One `make` on any machine with a
  C++17 compiler.
