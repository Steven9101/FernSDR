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

## Work shared between listeners

Beyond the channelizer, two more pieces of work depend only on what is heard
or seen, not on who hears or sees it, and are done once for everyone who
shares them:

- **Broadcast FM.** A WFM channel is 200 kHz wide, and its discriminator
  and decimation cost more than everything after them. The band keeps one
  demodulator per station, passband and de-emphasis (`core/shared_fm.h`), with its RDS
  decoder, and each listener tuned there copies the audio and does only its
  own squelch, volume and codec.
- **Waterfall rows.** Most listeners look at the whole band at the width
  their screen asks for. The band range-codes each row once per such view
  (`core/shared_waterfall.h`); a listener takes the shared rows from the next
  key row on, and codes its own while it cannot take every row.

## Modules and decoders

The receiver never talks to hardware. An input module
([MODULES.md](MODULES.md)) is a separate program, started by the band that
uses it, that sends samples through a pipe and reads commands as lines of
JSON; a driver that crashes or hangs takes its module down, not the
receiver, which starts it again unless the failure needs the operator, such
as a setting the module refuses. A decoder module gets a narrow channel cut
from a band's shared spectrum (`core/decoder_tap.cpp`, at about 12 kHz, a
power-of-two share of the band's rate) on a thread of its own per decoder, and sends back what it decoded; every
decode is checked before it is kept, shown or reported to PSK Reporter.

## Hours on the air

A band's `hours` (`core/band_hours.h`) are times of day in UTC, or times
relative to sunrise and sunset at the station's grid square. The radio
checks them once a second; a band whose hours end stops and its listeners
are moved to the band that takes over its input. Bands share one input when
their hours cannot overlap; hours by the Sun that might meet somewhere in the
year are allowed with a warning, and the band on the air first keeps the
input. Starting and stopping run on a thread of
their own, so a slow device never holds up a tick.

## What the server keeps

No database and no accounts. Beside the configuration, the receiver keeps
what the admin panel saves (`fernsdr-settings.json`), the page's look
(`fernsdr-theme.json`), the pictures uploaded for it, installed modules, the
waterfall archive of bands that keep one, and in memory the last day of
decodes (at most 20,000) and the chat's recent messages. A backup from the
admin panel carries the configuration without the machine's own sections
(`[server]`, `[modules]`, `[admin]`), the saved settings without the listing
id and the chat's mutes, the look, and the pictures (up to 64 files and
6 MB); it names the installed modules for another machine to install
again, and leaves out the archive, the decodes and the chat.

## What is deliberately absent

- **No TLS in the server.** A reverse proxy does it better, and the installer
  sets one up; see [DEPLOYMENT.md](DEPLOYMENT.md#https).
- **No external dependencies in the server.** One `make` on any machine with a
  C++17 compiler. Anything that needs a vendor library, such as SDRplay's API,
  is a module.
- **No hardware drivers in the server**, for the same reason: see
  [Modules and decoders](#modules-and-decoders).
- **No audio-mode decoders in the page.** Modes such as RTTY, PSK31 and SSTV
  reach software on the listener's computer intact through the audio; the
  receiver decodes only what is worth doing once for everybody, as FT8.
