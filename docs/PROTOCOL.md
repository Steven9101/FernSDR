# Protocol

In plain words: the listener's browser and the receiver talk over one
connection that stays open: the browser says what it wants to hear, the
receiver sends sound, the waterfall and the meter. This is that conversation,
message by message.

One WebSocket at `/ws`. Text frames carry JSON control messages; binary frames
carry audio, waterfall data and negotiated compact meter updates. Byte layouts are in
[`CODEC.md`](CODEC.md) and `server/src/core/protocol.h`.

There is also a small HTTP API: `GET /api/status` returns the site, its bands
and their listener counts as JSON; `GET /api/health` returns 200 while every
band is receiving, deliberately switched off by the operator or off the air
by its hours, and 503 otherwise. `GET /api/space-weather` returns what the
receiver last fetched for the space weather widget; it fetches only while
that widget is on the page, and answers `{}` until its first fetch.

The upgrade requires GET over HTTP/1.1, WebSocket version 13 and a canonical
16-byte base64 nonce as specified by [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455#section-4.2.1).

`GET /api/history?band=ID&from=MS&to=MS` reads an enabled archive. Times are
integer Unix milliseconds. The binary response starts with a four-byte
little-endian JSON-header length, then UTF-8 JSON and row-major byte levels.
The header supplies `bins`, `floor_db`, `ceiling_db`, `low_hz`, `high_hz`,
`oldest_ms`, `newest_ms`, `row_interval_ms` and the `times` array. The frequency
bounds describe the outer edges of the recorded spectral cells. FFT bin zero
is centred on the lowest sampled frequency, so the first cell starts half
a native FFT bin below it. These display bounds differ from `sample_low`
and `sample_high`, which limit the receiver's physical passband.
`row_interval_ms` includes the
sampling stride for long ranges. Responses contain at most 4,096 timestamps
and 1 MiB of level bytes. Two optional parameters ask for no more than a
picture can show: `rows=N` caps the rows, and `width=N` merges neighbouring
bins by their maximum until no more than about N remain (the header's `bins`
says how many came back), so a carrier one bin wide survives a narrow
picture. Timestamps retain gaps; rows must not be stretched
uniformly across the requested period. Private archives require an admin
session. Access changes are reported in `band-status`.

`GET /api/decodes` returns what the public decoders heard, newest last, as
`{"epoch":"...","through":N,"decodes":[...]}`. Each decode carries `seq`, `decoder`,
`channel`, `band`, `mode`, `time` (UTC start of its slot, Unix ms), `dial`,
`freq` (Hz above the dial), `snr`, `dt`, `message`, `quality` and, when the
module found them, `call`, `grid` and `report`. Without parameters it returns
the newest 500; `limit=N` (1 to 2000) changes that. `since=N` instead returns
the decodes numbered after N, oldest first; asking again with `since` set to
the answer's `through` continues without gaps or repeats. Decodes are kept in
memory and numbered from 1 at each start; `epoch` changes with every start,
and a page that sees it change, or `through` go down, starts over without
`since`. A decoder made public shows only what it decodes from then on. An
admin session sees every decoder's decodes; without one, a receiver with no
public decoder answers 404.

## Principles

- **The server is authoritative.** It clamps anything it cannot honour and
  replies with what it actually did. The client displays that, not what it
  asked for.
- **Invalid input is isolated.** Invalid commands receive an error. Invalid
  framing, UTF-8, oversized messages or sustained flooding close the sender.
- **Anything clamped is explained.** A client that asks for a 40 kHz passband
  is told why it did not get one.

## Client to server

A message is at most 8 kB, far more than the longest one here needs; a
longer one closes the connection. A key that appears twice in one object
makes the message invalid rather than letting either value win.

All messages are JSON objects with a `type`. Unknown fields are ignored, and
omitted fields keep their current value.

### `hello`

```json
{"type":"hello","capabilities":["meter-v1","meter-ctcss","nac2","nac3","audio-discontinuity"]}
```

Send the intersection of supported features and `welcome.capabilities`.
`meter-v1` selects the 26-byte binary meter; `meter-ctcss` lets it carry the
CTCSS tone in NFM (flag 16, the tone in tenths of a hertz in bytes 8 to 11,
where SAM puts its carrier offset; 0 for no tone), which a page without it
would refuse as an unknown flag; `nac2` permits compact audio metadata. Without them, meters remain JSON and audio uses the original NAC
layout. A new hello replaces the previous selection. Waterfall coding is
negotiated separately, and capabilities must be rediscovered on reconnect.
Changes apply to subsequent production and queue-expiry decisions. Frames
already queued or in flight retain their flags, so a client must still decode
the formats it previously selected on that connection.

`nac3` permits NAC3 packets (audio flag bit 3), which the server then sends
instead of NAC or NAC2 frames. Their size and quality come from the `audio`
command's `frames` and `noise_margin`.

`audio-discontinuity` permits audio flag bit 2. The sender can then discard
old, completely unsent audio and mark the first surviving frame. Without
this capability, audio retains the older byte-based queue limit.

### `tune`

```json
{"type":"tune","band":"40m","freq":7100000,"mode":"usb",
 "low":300,"high":2700,"cw_pitch":700}
```

`low` and `high` are the passband edges relative to the tuning point, so USB is
roughly `300 .. 2700` and LSB `-2700 .. -300`. Changing `mode` without naming a
passband adopts that mode's default - otherwise switching from AM to CW would
leave a 9 kHz CW filter.

Modes: `usb`, `lsb`, `cw`, `cwl`, `am`, `sam`, `nfm`, `dsb`, and `wfm` on a
band whose `wfm` is true in `welcome` (at least 240 kHz wide, and not
switched off by the operator). `fm` is taken for `nfm`. A listener asking for
`wfm` on a band without it gets `nfm` and a `note` saying so.

CW passbands are centred on `cw_pitch`; CW-L uses its negative. The displayed
signal frequency is `freq + cw_pitch` for CW and `freq - cw_pitch` for CW-L.
Filter presets change width around that pitch. Bounds are clamped to the
actual audio Nyquist frequency and captured RF range.

### `viewport`

```json
{"type":"viewport","enabled":true,"low":7000000,"high":7300000,
 "width":1024,"fps":12}
```

The waterfall span, the number of bins to render it into, and the line rate.
The server may deliver fewer lines than requested to hold the bandwidth
budget; the actual rate is reported in `meter`.

Clients supporting zero-run waterfall coding send `"codec":"wfc2"` in this
command. Without that negotiation, the server sends the original format.
Each line's flags still determine its encoding, so the server can choose
whichever representation is smaller.

If `welcome.capabilities` includes `wfc3`, clients may request `"codec":"wfc3"`
for additional predictors and native FFT grids when zoomed in. A received row
can have fewer bins and slightly wider frequency bounds than requested; its
own width and bounds remain authoritative. The requested viewport is unchanged.

If `welcome.capabilities` includes `wfc4`, clients may request `"codec":"wfc4"`
and `"step_db":2` in the same viewport command. This retains the WFC3
predictors and grid selection, but quantises levels in 2 dB steps. Omit
`step_db`, or set it to 1, for 1 dB steps. Include the codec when changing
precision. Each received row's flag bit 3 determines its precision, including
rows already in flight when the setting changes. Low and Balanced use 2 dB;
High uses 1 dB. Older servers retain their supported 1 dB format.

If `welcome.capabilities` includes `wfc5`, clients may request `"codec":"wfc5"`,
with `step_db` as for WFC4. Rows then arrive range-coded (flag bit 4, layout
in docs/CODEC.md), about a third smaller than WFC4 on a typical band. A
range-coded row that is not a key row depends on every row since the last key
row; after a sequence gap or a failed row, discard the decoder's state and
wait for the next key row, at most 24 rows away.

The server codes WFC5 rows once for every listener who asks for the same view
(span, width, `fps`, `step_db`, and whether native grids are allowed, which
WFC5 always does) and whose budget carries every row, and sends each of
them the same payload under its own header. A listener joins that shared
stream at one of its key rows and until then gets rows of its own; nothing in
the rows says which, and a client needs to do nothing about it. Asking for a
width many other pages ask for makes a listener cheaper to serve: the pages in
this repository round their canvas width down to a sixteenth of an octave
(1024, 1088, 1152 and so on: 64 pixels apart from 1024 to 2047, 32 from 512 to
1023, 16 below that) and stretch the rows by less than 6.25 %.

### `audio`

```json
{"type":"audio","enabled":true,"rate":12000,"bitrate":48000,"frames":2,"noise_margin":12}
```

`rate` is a ceiling. The actual rate is the band rate divided by a power of
two: the lowest such rate that still carries the passband, so SSB and CW run
at half or a quarter of what AM needs and cost that much less to demodulate
and code. A wider passband raises it at once; a narrower one lowers it only
with the next change of mode, since every change is a new stream. The rate
is rarely round and need not be whole, 7812.5 Hz from 64 Msps for instance;
the server reports it exactly in `audio-config` and the client resamples.

`bitrate` is a ceiling. NAC3 frames use what their quality target needs and
only reach it on busy signals. `frames` (1 to 4) is the number of NAC3 frames
per packet: each extra frame adds one frame of delay and saves a header and
most of a frame's side information. `noise_margin` (6 to 30 dB) is how far
below the channel noise NAC3 places its own; 12 adds 0.27 dB of noise. Both
apply only after `nac3` was negotiated.

### `dsp`

```json
{"type":"dsp","agc":"auto","gain":0,"nr":0.0,"autonotch":false,
 "squelch":-200,"notches":[{"hz":1200,"width":150}],
 "highpass":0,"deemphasis":300,"ctcss_filter":true,"ctcss_squelch":0}
```

`agc` is `auto`, `fast`, `medium`, `slow`, `long` or `off`. The automatic
profiles set the gain against the band noise around the channel, which they
keep 15 dB under their target, and differ in how long they hold their gain
after the signal: 0.3, 1, 2.5 and 5 seconds. `auto` is the server's
recommendation, `slow` at present. `steady`, the name of an earlier build's
profile, is taken for `slow`; an unknown name is answered with an `error` and
the rest of the message still applied. NFM runs without gain control
whatever `agc` says. `nr` runs 0 to 1. `squelch` is in
dBFS; -200 is fully open. At most eight notches. `highpass` cuts audio below
that many hertz after demodulation, 0 to 1000, with 0 off; a second-order
filter, for hum on AM and the subaudible tones under NFM speech.
`deemphasis` is the NFM de-emphasis time constant in microseconds, 0 to 2000;
0 is flat, for a decoder that wants the signal as sent, 300 the default and
750 the land-mobile standard. Servers that predate the two ignore them.
`ctcss_filter`, true unless set, takes the NFM station's CTCSS tone out of the
audio with a notch about 10 Hz wide at its measured frequency, once the tone
has been measured (about two and a half seconds after it starts).
`ctcss_squelch` is a tone squelch for NFM: one of the 50 standard CTCSS
tones in Hz keeps the audio muted until that tone is received (it opens in
about half a second and ignores the neighbouring tones), 0 or anything that
is not a standard tone switches it off.

Four more fields: `max_gain` caps what the automatic gain may add, in dB
(60 unless set); `volume` scales the audio, 1 being unity; `auto_squelch`,
true or false, switches the automatic squelch, which needs no level;
`wfm_deemphasis` is the broadcast FM de-emphasis in microseconds, 0 to 200,
50 unless set (75 in the Americas, which the page chooses from the band
plan's region).

### `state` (request)

```json
{"type":"state"}
```

Asks for a `state` reply without changing anything.

### `chat`

```json
{"type":"chat","name":"Ann","text":"strong on 14074 FT8"}
{"type":"chat","history":true}
```

Posts a line to the chat, under `name` (cut to 24 bytes of UTF-8, kept for
the connection's later lines). A longer line is cut to 400 bytes; the
receiver limits how often one address may post. With `history`, nothing is
posted: the reply is a `chat-history` with the messages kept, which a page
asks for whenever its chat appears.

### `ping`

```json
{"type":"ping","t":12345}
```

Answered with `{"type":"pong","t":12345}`.

### `active`

```json
{"type":"active"}
```

The listener answered the `inactivity` question. It counts as activity, as
`tune`, `dsp`, `audio`, `chat` and a `viewport` that moves the span do;
pings, `state` and a viewport that only changes its width do not.

## Server to client

### `welcome`

Sent once on connect: site metadata, the band list, the available modes, and
the limits the server will enforce.

```json
{"type":"welcome","protocol":1,"session":7,"capabilities":["meter-v1","nac2","nac3","wfc3","wfc4","wfc5","audio-discontinuity"],
 "site":{"name":"...","operator":"...","location":"...","grid":"...","band_plan":"auto",
         "antenna":"...","contact":"...","website":"...","notice":"..."},
 "bands":[{"id":"40m","name":"40 m","center":7100000,
           "low":7023200,"high":7176800,"sample_rate":192000,
           "sample_low":7004000,"sample_high":7196000,
           "max_bandwidth":12000,"receiver":"rx888","listeners":3,"running":true}],
 "modes":["usb","lsb","cw","cwl","am","sam","nfm","dsb"],
 "limits":{"max_waterfall_width":4096,"max_waterfall_fps":30,
           "min_audio_bitrate":8000,"max_audio_bitrate":128000,
           "max_users":200}}
```

Besides the fields shown, a band carries `wfm: true` when it offers wide
FM, `noise_blanker` (its level, for the page to show), `noise_floor` (dBFS),
`calibration` (the S-meter's `[{hz, offset}]` points, empty until the
operator measured it), and `history`, `off`, `private` or `public`, with
`history_from` and `history_to` (Unix ms, 0 while nothing is kept yet)
whenever `history` is not `off`. The
message also carries `theme` (the operator's look and widgets, as the
`theme` message does), `agc_profiles` (the `agc` names the server takes),
and in `site` `source_url` (where this build's source is) and `chat`
(whether the chat is on).

Every band carries `on_air`: whether its hours have it on the air. A band
with hours also carries `hours` (the operator's text, `sunset-sunrise`) and
`next_change`, the UTC milliseconds at which `on_air` next flips, -1 when it
does not in the next nine days. Bands that take turns on one input by their
hours carry the same `shared_input` id, so a page can name the band that
comes on as another goes off. Off the air, a band is stopped on purpose: a
`tune` to it is not refused but goes to the band on the air on its input, or
stays on the current band, and the `state` reply says so in its `note`. When
a band goes off the air, its listeners are moved the same way.

A band's `receiver` says what it listens with, for the label beside its
name: the source kind (`test`, `file`, `pipe`, `stdin`, `udp`) or, for a
hardware module, the module's id (`rx888`, `rtlsdr`, `sdrplay`). Never a
serial number or a path.

When the operator has made a decoder public, `decoders` lists each one and
the channels it listens to, `low` and `high` being the audio it covers above
the dial:

```json
"decoders":[{"id":"ft8","channels":[{"id":"40m-ft8-7074","band":"40m","mode":"ft8",
             "dial":7074000,"low":0,"high":4000}]}]
```

Without a public decoder the key is absent. The `station` message, sent
when the operator changes the configuration, always carries `decoders`, as
`[]` when none is public, so a page can drop its decodes view.

### `state`

The authoritative receiver state, sent after every change. Carries an optional
`note` when a value had to be clamped.

```json
{"type":"state","band":"40m","freq":7100000,"mode":"usb",
 "low":300,"high":2700,"filter_limit":6000,"cw_pitch":700,"agc":"auto","agc_effective":"slow","gain":0,
 "nr":0,"autonotch":false,"volume":1,"squelch":-200,"highpass":0,"deemphasis":300,"ctcss_filter":true,
 "audio_enabled":true,"audio_bitrate":48000,"audio_rate":12000,
 "audio_codec":"nac3","audio_frames":2,"noise_margin":12,
 "notches":[],
 "viewport":{"enabled":true,"low":7023200,"high":7176800,
             "width":1024,"fps":12},
 "note":"passband narrowed to the channel maximum"}
```

The reply also carries `auto_squelch`, `ctcss_squelch` and `wfm_deemphasis`
as the `dsp` command sets them.

`tune`, `viewport` and `dsp` commands may each carry a monotonically increasing
`request_id`. State replies include `ack: {"tune":N,"viewport":N,"dsp":N}` with
the most recently processed ID for each group. Clients should apply each
group only when its acknowledgement catches up with their latest local edit.
This prevents a delayed reply from moving a view backwards during a gesture.
`filter_limit` is the largest absolute audio cutoff the current channel can
represent. `agc_effective` is what `agc` comes to in the current mode: the
profile `auto` stands for, or `off` for NFM. `sample_low` and `sample_high` describe captured RF bounds; the
band's `low` and `high` describe its operator-selected usable range.

### `band-status`

```json
{"type":"band-status","bands":[{"id":"40m","listeners":3,"running":true,"on_air":true,"next_change":-1}]}
```

Per-band counts and source availability are checked once per second and sent
only when they change. `running` means samples are arriving: a band whose
hardware module is being restarted is `false` until it receives again. Merge by band ID. A meter's `listeners` field is the
total across the receiver, so it must not overwrite a band's count.
The update also carries `history` access when that changes, so an archive
enabled by the operator appears without a page reload, and `on_air` and
`next_change` as the band's hours move on.

### `audio-config`

Sent whenever the audio stream's configuration changes.

```json
{"type":"audio-config","generation":3,"rate":12000,
 "frame_samples":128,"bitrate":48000}
```

`generation` is the 4-bit value carried in each audio frame's header. Putting
the mapping here rather than a sample rate in every frame saves about
3 kbit/s at 94 frames a second, for information that changes once an hour.

`rate` is exact and may have a fractional part.

The server sends this message before any audio frame using the new format,
including a band change. Clients must also notice a rate change if the
4-bit generation happens to repeat.

### `meter`

About ten times a second.

Under sustained transport backpressure, meter updates fall to two per second
until the link has cleared for three seconds. Control replies and audio
configuration messages retain their usual priority.

```json
{"type":"meter","dbfs":-92.4,"gain_db":18.0,"squelch_open":true,
 "pll_locked":true,"pll_offset":-12.5,
 "audio_bps":48210,"waterfall_bps":35400,"waterfall_fps":12,
 "listeners":37}
```

`dbfs` is measured before the AGC, so it reflects the antenna rather than the
gain control. `pll_locked` and `pll_offset` appear only in SAM, `ctcss` (the tone in Hz, 0 for none) only in NFM. `waterfall_fps`
is the rate actually being sent, which may be below the requested one.

### `rds`

What the broadcast FM station tuned sends by RDS, as far as it has been
received. Sent when it changes: at once for another station (or a band or
mode without it), so a neighbour's name is never shown on it, and otherwise at
most once a second, so a station's radiotext arriving piece by piece does not
become a stream of messages.

```json
{"type":"rds","pi":"D3C3","pty":10,"tp":true,
 "ps":"FERN FM ","rt":"Now playing: a synthetic station"}
```

Only what has been received is present: `pi` (the programme identification,
four hex digits), `pty` (programme type, 0 to 31; its name depends on whether
the RDS or the North American RBDS table applies), `tp` (traffic programme),
`ps` (the station name, eight characters, often padded with spaces) and `rt`
(radiotext, up to 64 characters). Both texts are UTF-8, converted from the RDS
character set. A message with none of them, `{"type":"rds"}`, means there is
nothing to show: another station, another mode, or a station without RDS.

The server decodes RDS once per station, beside the shared WFM demodulation,
so it costs the same for one listener as for a thousand.

### `chat`, `chat-history` and `chat-refused`

```json
{"type":"chat","id":42,"name":"Ann","text":"strong on 14074 FT8","at":1790000000000}
{"type":"chat-history","messages":[{"id":41,"name":"Bo","text":"...","at":1789999990000}]}
{"type":"chat-refused","reason":"you are sending faster than anyone can read; wait a moment"}
```

Every listener gets each posted line as `chat`, with `at` in Unix
milliseconds. `chat-history` answers a `chat` with `history`. A line the
receiver would not take (empty, too fast, from a muted address, or with the
chat switched off) is answered with `chat-refused` and its reason, to the
sender only.

### `station` and `theme`

```json
{"type":"station","site":{"name":"...","chat":true,"source_url":"..."},"decoders":[]}
{"type":"theme","theme":{"colors":{},"widgets":[]}}
```

Sent to every listener when the operator changes the station's details or
the page's look: `station` carries the same `site` object as `welcome` and
the public `decoders`, `theme` the whole theme as `welcome` carries it. A
page applies them as they arrive.

### `error`

```json
{"type":"error","message":"unknown mode 'am-wide'"}
```

The connection stays open.

### `inactivity`

```json
{"type":"inactivity","seconds":60}
```

The station set a listener timeout (`listener_timeout` in `[site]`, minutes)
and this listener has been inactive long enough that their place will be
freed in `seconds` unless they do something or answer with `active`. At the
limit the server closes the connection with code 4001 and a reason such as
`no activity for 60 minutes`. A client must not reconnect by itself after
4001, since that would take the place straight back; it offers to listen
again instead.

## Binary frames

### Audio - first byte `0x01`

```
offset  size  field
     0     1  0x01
     1     1  flags: bit 0 muted; bit 1 NAC2; bit 2 discontinuity;
                       bit 3 NAC3 packet; bits 4-7 generation
     2     2  sequence, little-endian, wraps at 65536
     4     n  NAC frame, or a NAC3 packet of one to four frames
```

The sequence counts frames. A NAC3 packet carries the sequence of its first
frame; its frame count is the top two bits of its first payload byte plus one,
and the next packet's sequence follows by that many. Bits 1 and 3 are never
both set.

The payload is a valid frame even when muted. Ordinary sequence gaps tell the
client how many frames to conceal. With bit 2 set, the sender deliberately
discarded stale audio: clear the decoded receive queue and MDCT overlap,
skip concealment for that sequence gap, and resume from this frame. Keep
the learned jitter target; this does not change the sample rate or generation.
Reject duplicates before applying the marker. Bit 2 is independent of NAC2,
mute and generation, and adds no bytes to a frame.

Before each socket write, the server removes eligible media frames that have
waited at least 250 ms in its connection queue. A partly written WebSocket
frame must finish unchanged. Text, HTTP, meters and WebSocket control frames
retain FIFO order. This expiry applies only to that queue: listener outboxes,
partially sent frames, kernel buffers, intermediaries and client playback
add their own delay.

### Waterfall - first byte `0x02`

```
offset  size  field
     0     1  0x02
     1     1  flags: bit 0 zero runs; bit 1 extended predictor;
                       bit 2 native grid; bit 3 two-decibel steps;
                       bit 4 range-coded row (WFC5; bits 0 and 1 clear)
     2     2  sequence, little-endian
     4     8  low edge, Hz, IEEE 754 double, little-endian
    12     8  high edge, Hz
    20     2  width in bins, little-endian
    22     n  compressed line
```

The span travels with every line rather than being implied by the last
viewport command. A line in flight when the user pans would otherwise be drawn
in the wrong place, which shows up as the waterfall tearing sideways during a
drag.

Native-grid rows contain 2 to 4096 bins; ordinary rows contain 16 to 4096.
The native-grid and precision flags are independent of the predictor flag.
Width, grid-bound, precision or sequence changes invalidate temporal prediction. The server sends an
independent row after changing the view or dropping a row, and at least once
every two seconds. After queue expiry, already encoded dependent rows are
also withheld until an independent row survives. Waterfall bits 4 through 7
are reserved. Bitstream layouts and invalid encodings are specified in
[`CODEC.md`](CODEC.md).

### Meter - first byte `0x03`

Exactly 26 bytes, all multibyte integers little-endian:

| Offset | Type | Field |
|---:|---|---|
| 0 | u8 | `0x03` |
| 1 | u8 | Flags: bit 0 squelch open, bit 1 PLL locked, bit 2 automatic squelch statistic present, bit 3 SAM fields present, bit 4 NFM (only after `meter-ctcss`) |
| 2 | i16 | dBFS × 10 |
| 4 | i16 | AGC gain in dB × 10 |
| 6 | u16 | Automatic squelch statistic × 10, when bit 2 is set |
| 8 | i32 | PLL offset in Hz × 10 when bit 3 is set; the CTCSS tone in Hz × 10 when bit 4 is set, 0 for none |
| 12 | u32 | Audio payload bits/s |
| 16 | u32 | Waterfall payload bits/s |
| 20 | u16 | Waterfall lines/s × 10 |
| 22 | u32 | Listeners across the receiver |

Bits 5 to 7 are reserved and must be zero. Ignore optional fields unless their
presence flag is set. Reject incorrect lengths and unknown flag bits. Rates
count media payload; they exclude WebSocket framing and control messages.
Ten updates per second need 2.24 kbit/s including WebSocket framing.

## Reconnection

The client reconnects with exponential backoff and jitter - so a receiver
coming back up is not hit simultaneously by every client it dropped - and
replays its settings on the way back in. A dropped connection should cost a
second of audio, not the user's place on the band. The exception is close
code 4001, the listener timeout (see `inactivity`), after which it waits for
the listener.
