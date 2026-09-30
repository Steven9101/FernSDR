# Performance

In plain words: a receiver does some work once for the whole band, and some
for each listener, and sends each listener a stream of data. How many
listeners one machine carries depends on which of those runs out first,
which depends on the band, the listeners and the machine; this page says
where the work goes and how to measure your own.

[TESTING.md](TESTING.md) says how everything is checked. Capacity depends
on the input bandwidth, listener modes, waterfall settings, hardware and
uplink. A listener count without those
conditions is not a useful sizing guide.

## Where the work happens

Each band reads its input and shares one channelizer transform across its
listeners. A listener extracts a small slice, demodulates it and encodes the
audio. Transforms use split real/imaginary arrays. Up to 16,384 points
(32,768 with AVX-512) they run in four steps: each butterfly works on a
vector of independent columns or rows, with one transpose between the two
passes, and the lengths up to 256 points that listeners use are unrolled at
compile time. Longer ones, up to 131,072 points on SSE2 and NEON, 262,144
on AVX2 and 524,288 with AVX-512, start with one radix-16 pass over the
whole array that leaves sixteen blocks small enough to finish in L2; the
longest stream through radix-4 passes. All use four-lane vectors on SSE2
and NEON; runtime CPU detection selects eight lanes with FMA on AVX2 x86
machines, sixteen with AVX-512 on AMD, and the binary still runs on older
x86.

A band's front end goes straight into the first pass: an IQ band's
interleaved blocks and a real band's sample pairs are split, packed and
windowed as they are read, so no history slides and no windowed copy is
made. A real transform's bins come out of its last pass already untangled
from the packed half-length transform, and in the long plans the
channelizer's sine window is computed from tables of a sixteenth of its
length instead of read.
Other targets run the same kernels one lane wide. Plans of one length share
their twiddle tables. The arrays a band's transforms sweep on every block are
allocated on 2 MiB boundaries and offered to transparent huge pages from
1 MiB on (`src/dsp/dsp_buffer.h`), which saves the TLB misses of long
strided passes.
The backend has no external DSP or codec libraries.

Large independent channelizer and waterfall transforms can run concurrently.
Groups of 64 or more listeners share a bounded worker pool. For FFT sizes of
262,144 and above, the producer can transform the next block while workers
finish the previous listeners. One retained spectrum per band bounds that
overlap. Its bins and sample index stay together, so advancing the producer
does not change a listener's phase correction. Completed audio wakes the
network immediately, even if the next source read is blocked. EOF, stop and
exceptions drain the batch before releasing its storage.

Small bands and small listener groups keep their direct processing path.
The extra spectrum costs 8 MiB at a 1,048,576-point FFT. Real-input packing
also removes a windowed copy and halves the overlap history, saving 6 MiB
at that size. Those are buffer sizes, not total process-memory estimates.

The default worker count reserves one CPU for networking and one per band,
then uses up to eight remaining CPUs. CPU affinity is respected. A container's
CPU quota can be smaller than its affinity mask; set `[server] dsp_workers`
explicitly in that case. Values are 0 through 32; 0 disables the pool.

The waterfall builds a peak pyramid once per band. Each viewport selects a
level close to its pixel resolution rather than scanning the full source
spectrum. Narrow carriers remain visible when zoomed out. A second pyramid
holds each line averaged with the one before, and a listener showing half the
band's lines or fewer draws from it instead of skipping every other line: on
recorded 40 m and 20 m bands at 12 lines/s that took the waterfall from 28.3
and 28.7 to 26.0 and 26.2 kbit/s, with less speckle. Increasing the
source FFT still costs CPU and memory even when clients receive only 1,024
bins. History writes bounded rows to disk and is disabled unless configured.

## Size a receiver

Start with the intended sample rate and a representative signal. Run the
load test against a private receiver, leaving capacity for HTTP requests:

```ini
[site]
max_users = 1100
[server]
max_connections = 1200
```

```sh
python3 tools/loadtest.py --port 8073 --clients 1000 --seconds 60 \
  --codec wfc4 --waterfall-step 2 --audio-codec nac2 --meter-format binary \
  --modes usb,lsb,cw,cwl,am,sam,nfm,dsb
```

Check delivered audio seconds as well as sequence gaps. A source that runs
at half speed can deliver every frame and still fail to provide live audio.

`loadtest.py` speaks the waterfall codecs up to WFC4. Pages ask for WFC5,
which is range-coded and which the server codes once for every listener
with the same view, so the tool's waterfall figures are not a page's: WFC5
rows are about a third smaller, and their CPU cost depends on how many
listeners share a view. Measure with real pages where the waterfall's exact
cost matters.

The tool reports CPU, RSS, media payload and TCP payload separately. TCP
payload includes WebSocket/control bytes, but excludes IP, TCP and TLS
overhead. Leave uplink headroom for those costs and traffic bursts.

For a wideband real source, `sample_rate = 64M` covers DC to 32 MHz before
the usable-range setting. A 1,048,576-point FFT resolves about 61 Hz and
advances in 8.192 ms blocks. Source conversion, transforms, listener work and
networking all have to keep up. The channelizer's transform starts with
the first listener and is most of the band's work: on six cores of a Ryzen 9
9950X in a virtual machine the band took 5 % of a core with nobody listening,
31 % with one listener and 33 % with twelve, about 0.2 % for each listener
after the first.

The band's own waterfall line has 32,768 bins of about 1 kHz. A listener
zoomed in further gets a spectrum of their own view from that same transform,
with bins no wider than a pixel (down to 2 Hz): for views from about 115 kHz
up its bins read through a sin^3 window, summed in tiles of 1,024 that every
view over them shares; below that, a narrow channel of it. Timed on its own,
that is 0.02 to 0.06 % of a core per view from 150 to 600 kHz, 0.15 % at
110 kHz, the dearest width, 0.05 % at 30 kHz and 0.01 % at 2 kHz; twelve
listeners on 300 kHz views of 40 m cost 0.014 % each. A view of its own also
means rows coded for that listener alone rather than shared, which took
about 0.2 % of a core per listener more on the whole. Listeners on the same
view share both. Raising `spectrum_rate` or `spectrum_averages`
increases transform work; reducing the waterfall cadence keeps the frequency
resolution and gives up time detail. Measure that configuration instead of
extrapolating from a narrowband receiver or an isolated FFT benchmark.

## Network and audio

`max_user_bitrate` budgets audio, waterfall and measured status/control traffic.
The controller reserves space for control frames, using their encoded byte
counts and WebSocket headers, including broadcasts. This is an adaptive
average budget; a burst of control replies can briefly exceed it. Congestion reduces
waterfall traffic first. Persistent queued audio also reduces its bitrate.
A stall, where the queue fills at once, does not count against the link:
once the queue has been empty for a second, both return to the level the
link last carried for ten quiet seconds, or to the starting level. A queue
that fills slowly lowers that level. So does one that follows the budget's
own step up within three seconds, which also makes the next step up wait
longer; the first return after a stall that meets a sudden queue is let off
once. Below that level, recovery proceeds step by step. A queue in the network
beyond the socket shows in the connection's round trip: 50 ms over its recent
floor stops the budget from probing and the waterfall from returning, and
150 ms over it takes the waterfall but not the audio. After a stall the audio
returns once the socket is clear and the round trip is under 150 ms over its
floor. The round trip just after a hold, when Wi-Fi
retries or a cell change deliver held packets late, measures the hold rather
than a queue and is not counted. Negotiated binary meters avoid frequent JSON
telemetry. WFC3 selects spatial and temporal predictors while preserving the
1 dB waterfall levels. Narrow views send native FFT bins instead of repeated
interpolated samples. NAC2 packs audio metadata more tightly without changing
the reconstructed samples. Both formats retain a smaller original payload
when the additional representation would cost more.

WFC4 lets Low and Balanced use 2 dB shading steps, while High retains 1 dB.
This changes amplitude precision, not frequency resolution. Narrow carriers
still enter the same peak-selected bins. The limit is 1 dB level error inside
the codec range, compared with 0.5 dB for the original quantiser.

The Stream panel measures received messages in the browser, including
WebSocket headers, and separates audio, waterfall and status/control traffic.
The total excludes TCP/IP, TLS and retransmissions. It is a measured rate,
not a fixed promise attached to a profile. Server meters describe production
over the source sample clock and can differ during a delivery stall or burst.

Use `--codec wfc2 --audio-codec nac` to measure the older formats. The newer
formats change both bytes and processing cost.

The browser adjusts its playout buffer from arrival timing, conceals short
gaps and trims persistent backlog. A larger buffer trades latency for more
time to recover. It cannot provide current audio during an outage. Test the
actual access network, including handoffs and upload contention.

```sh
cd web
node tools/link-check.mjs http://127.0.0.1:8073/ 48 90
FERNSDR_LINK_MAX_RECOVERY_MS=2000 node tools/link-check.mjs http://127.0.0.1:8073/ 64 55 10 8000 80
node tools/interaction-check.mjs http://127.0.0.1:8073/
```

The second link check adds an eight-second delivery pause after ten seconds.
The remaining arguments are rate in kbit/s, duration in seconds, pause start
in seconds, pause length in milliseconds, and periodic jitter in milliseconds.
It measures both decoded buffer occupancy and growth in transport delay from
the audio sequence clock. The optional recovery bound checks how quickly that
delay returns below 500 ms above its baseline after delivery resumes. Neither
metric is absolute antenna-to-speaker latency. `FERNSDR_LINK_TRACE` saves both
traces as JSON. The relay shapes a local slow reader, not TCP loss or mobile RTT.

The default audio setting is an SSB bitrate ceiling. CW requests less; AM and
NFM request more, within the band's budget. These weights do not establish
equal perceived quality across modes. The quality harness reports codec
distortion against encoder input separately from the complete receive chain:

```sh
make -C server quality
server/build/audio-quality --mode usb --seconds 8 --sweep
```

Its synthetic AM/NFM signal clock and LSB reference were corrected during
this work. Earlier per-mode quality tables are not valid comparisons.
Codec SNR describes coding error, not an improvement in antenna SNR.
Narrow CW has an additional low-rate filter before AGC; every filter still
has a finite transition band.

## Measure the parts

```sh
make -C server bench
server/build/benchmark wide
make -C server fft-bench
make -C server fft-compare FFTW_PREFIX=/opt/fftw   # optional, needs FFTW 3
python3 tools/measure-latency.py --help
cd web
node tools/soak.mjs --url http://127.0.0.1:8073/ --minutes 30
```

`fft-bench` times each transform length the receiver uses and its error
against a double-precision transform. `fft-compare` builds the same program
beside FFTW 3 in single precision; FFTW is linked into that tool only.
`FERNSDR_FFT_ISA=avx2`, `sse2` or `scalar` lowers the instruction set the
transforms use, for these tools and for the tests; `avx512` asks for it on
an Intel CPU that has it (see DEPLOYMENT.md).

The benchmark excludes source I/O, the server waterfall and network delivery.
The latency tool measures input-to-decoded-audio timing, excluding the
browser and output device. Run timing measurements without competing builds.
A short load or soak test cannot establish 24/7 reliability.
