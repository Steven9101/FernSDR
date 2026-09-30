# Method

How the numbers in the README's table and in `runs/20260929-final` were made.
Everything named here is in this directory, so any figure can be made again.

## The receivers

Eleven receivers, each built or pulled at a pinned version with its own
configuration (`receivers/<id>/`: Dockerfile, `receiver.json`, configs,
page adapter; each receiver's notes on its quirks and the bugs found in it
are published once those bugs are with its maintainers). The version is what an operator following the project's own
install instructions gets on the day. VertexSDR, NovaSDR and PhantomSDR-Plus
(phasor-labs) are earlier projects of FernSDR's author; every adapter,
configuration and driver here was written by that author too.

Two configuration tiers: *documented* is the project's own example or
default configuration, changed only for the input, the frequency, ports and
offline settings; *matched* also raises listener caps and per-address
limits (for capacity). Results say which tier they come from.

## The input

`source/scene.py` renders a scene file into IQ samples from a seed:
carriers, keyed markers, SSB, CW and noise, placed exactly (levels to
0.05 dB, frequencies to the hertz, marker edges to a microsecond, all
tested in `source/test_scene.py`). `source/pace` hands them to the
receiver at the sample rate, in 4 ms transfers, dropping and counting whole
transfers rather than slowing down when a receiver falls behind. Every
receiver that takes a 2.048 Msps IQ stream (as from an RTL-SDR, cu8) gets
the same bytes for the same seed; ka9q-web and UberSDR, which expect a
direct-sampling front end, get the same signals as a 20.48 Msps real band,
and are a separate input cohort for anything that depends on the input.
Run *i* of every receiver uses seed *i*.

## Isolation and fairness

One receiver runs at a time, in containers with no network but a veth on a
lab bridge that the host cannot reach and that has no route out
(`net/lab-net.sh`, checked by `net/check_isolation.py`), on two cores
(4 and 5) with 1 GB of memory, a read-only root and capped logs. Its
listeners are headless Chromium, each in a namespace of its own, playing
into the lab's own PulseAudio null sink (headless Chromium's built-in audio
output ran its clock several percent slow under load). A tap in the page
records, losslessly and without changing the page, what reaches the audio
output. The lab's browser bypasses Content-Security-Policy so that the tap
can load; nothing else is injected.

A run is also stopped when less than 1 GB of memory is left on the
machine. Nothing is dropped for its result: rejected runs are kept and
listed (see Validity).

## The session

Each receiver is started once per repetition and serves twelve listener
jobs, four at a time (`harness/links.py`), on one combined scene
(`scenes/all.scene`, 29 signals; `all-real.scene` for the real-band
receivers). While it runs, the receiver's CPU and memory are sampled every
second and tagged idle (input flowing, nobody listening) or with the batch
of listeners connected. Every job is a real browser, 30 s of measurement
after a 10 s warm-up:

- **listen**: tuned to 7.1592 MHz USB, where a marker is keyed every 0.5 s:
  a carrier with a 31-chip code, each marker's code telling which one it
  is, so that latencies of seconds are read without ambiguity. A channel of
  known noise audio next to it shows dropouts. This job gives the latency,
  the payload per stream and direction (the adapter says which socket or
  frame is audio or waterfall) and the page load. The other jobs use a
  800x600 window to spare the client cores; two pages that change their
  layout there (UberSDR, OpenWebRX+) get 1280x800, stated in their
  `receiver.json`.
- **digital**: 7.074 MHz USB with an FT8 ladder from -24 to -4 dB, RTTY at
  -6 and +4 dB and CW at +10 and +20 dB (SNR in 2.5 kHz), decoded by WSJT-X's
  jt9, minimodem and multimon-ng from what the page played, and from an
  ideal receiver's audio of the same scene (`digital/oracle-130s.json`).
- **tones**: 7.080 MHz USB, ten equal carriers heard as tones from 200 to
  3200 Hz (`analysis/quality.py`): passband; SNR, the tones between 300 and
  2700 Hz against the noise 25 Hz or more from any tone; SINAD, against all
  that is not a tone, so that a tone smeared by a wandering playback speed
  counts; pitch error, scale error and pitch wander, tracked in 1 s windows.
  An ideal receiver rendered from the scene is measured the same way.
- **links**: the listen job again on an emulated link, each on its own
  namespace's veth in both directions: 2 % loss, Wi-Fi micro-stalls, a 1 s
  hold or loss every 10 s, a cell change every 20 s, 600 ms round trip,
  32 and 24 kbit/s with a 150 ms queue, and a 15 s outage. Timed impairments
  start with the window and are logged. The links are shaped once the page
  has loaded and tuned: a page of a few hundred kilobytes takes minutes at
  24 kbit/s. What a page fetches in its first seconds, a typeface say, thus
  comes in unshaped; page weight is measured by the requests, not by how
  long it took here.

**Latency** is from the moment a sample leaves `pace` to the moment the
browser expects it at its audio output (`getOutputTimestamp`). An
independent trace of the null sink showed that estimate off by up to
9.3 ms, so absolute latencies carry that uncertainty. The stages before
`pace` and after the browser, a real radio's USB transfer and the sound
card, are not measured. After the outage, a
receiver counts as recovered when a marker is heard within 1 s of the
latency it had before, not when anything is heard again.

## Validity

A job is invalid, and listed, when a gate fails (the browser's audio clock
more than 0.1 % off, a hidden page, holes in the tap, a failed tuning
read-back or a mode command sent inside a receiver's debounce, a pitch more
than 20 Hz off, fewer than 20 frames a second) or when `pace` could not
hand the receiver its input inside the job's window: dropped because the
receiver fell behind, because nothing had the FIFO open, or queued for a
receiver that went away. Input lost outside the windows is reported as a
finding (a stall when listeners connect, a source that only runs while
someone listens). On an impaired link a page that plays nothing is the
receiver's result, not an invalid job. A round where the machine was
overloaded (several receivers at once, load 24) was thrown away whole and
is not part of the results.

## Capacity

`harness/capacity.py`: a browser listens for 20 s, then light clients
(`load/replay.mjs`) repeat what that page sent, at the same moments, each on
a frequency of its own where the protocol allows it and from a source
address of its own (200 addresses, since receivers limit what one address
may open), with the Origin a page sends. Steps of 1, 10, 50, 100, 200 and
400 listeners, 15 s windows after the ramp. A step passes when 95 % of the
listeners get at least 90 % of the browser's audio messages a second (not
bytes: a variable-rate codec spends fewer bytes on a quieter frequency) and
none is disconnected. Caps an operator would raise are raised (the matched
tier). The receiver keeps its two cores; the clients have four. A receiver
stops at the first step it fails. FernSDR alone was also tried at a single
step of 1,600 listeners (`runs/20260929-capacity-R1y`); no other receiver
was tried above 400, so the limits of FernSDR and of PA3FWM's WebSDR,
which both passed 400, are not known.

## Statistics

Each figure is the median of the valid runs with the minimum and maximum
beside it. Every receiver was measured three times in the published round;
with so few runs, a difference smaller than the spread a receiver's own
runs show is not a difference. Rejected runs are listed with the gate that
rejected them.

## What this lab cannot say

No real radio hardware, no ARM board, no phone: real-antenna behaviour,
dongle overload, USB latency, Raspberry Pi capacity, mobile browsers and
real cellular paths are not measured here. Stability over days is not
claimed for anyone.
