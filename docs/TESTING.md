# Testing

How FernSDR is checked before a release, and how to run the same checks on
your own machine. [PERFORMANCE.md](PERFORMANCE.md) says where the work goes
and how to measure your own receiver.

## The test suites

```sh
make -C server -j4 test
cd web && npm run typecheck && npx vitest run
```

`make -C server test` runs every server test, then the vector kernels again
once for each instruction set the machine has (AVX2, SSE2, scalar), so a
fault in one of them cannot hide behind another. It covers the DSP against
known answers (filters, demodulators, the gain control, the codecs, the
FFT), the wire protocol, HTTP and WebSocket parsing, the admin API and its
signatures, module and decoder lifecycles, the band schedule and sun times,
the updater and the installers' file handling.

Threaded and parsing code is also run under the sanitizers:

```sh
make -C server -j4 BUILD=build/tsan OPT="-O1 -g -fsanitize=thread" \
    LINK_FLAGS="-fsanitize=thread" build/tsan/fernsdr-tests
setarch -R server/build/tsan/fernsdr-tests
make -C server -j4 BUILD=build/asan OPT="-O1 -g -fsanitize=address,undefined" \
    LINK_FLAGS="-fsanitize=address,undefined" build/asan/fernsdr-tests
server/build/asan/fernsdr-tests
```

`make -C server fuzz` builds fuzzers with clang's libFuzzer for everything
that parses outside input: the configuration, JSON, HTTP, WebSocket frames,
module packages, release manifests, tar archives and Ed25519 signatures.

## Other machines

`make -C server release ARCH=x86_64|aarch64|armhf` builds the static release
programs, and `make -C server release-test` runs the whole test suite built
the same way, the ARM builds under qemu-user on a Cortex-A53 and a
Cortex-A7. qemu checks instructions and arithmetic, not speed or USB. The
x86_64 program runs on C libraries back to glibc 2.17 (CentOS 7).

## The page in a browser

The browser checks drive the real page with Playwright against a receiver
with a test band, and compare what it shows and sends:

```sh
cd web
node tools/controls-check.mjs http://127.0.0.1:18073/
node tools/interaction-check.mjs http://127.0.0.1:18073/
node tools/compatibility-check.mjs http://127.0.0.1:18073/
node tools/bands-check.mjs http://127.0.0.1:18075/
FERNSDR_ADMIN_PASSWORD=... node tools/admin-check.mjs --url http://127.0.0.1:18073/admin.html
node tools/link-check.mjs http://127.0.0.1:18073/ 48 90
node tools/behaviour-check.mjs http://127.0.0.1:18077/ /tmp/fernsdr-behaviour
```

Changes a listener can see are also looked at by eye, in the light and dark
themes and at phone width.

The behaviour check needs a receiver of its own, since a notice and a set of
widgets change the layout the other checks measure:

```ini
[site]
name = Behaviour check
operator = Test operator
location = Test location
antenna = Test antenna
notice = Maintenance tonight at 22 UTC
theme_file = behaviour-theme.json

[band:20m]
name = 20 m
source = test
sample_rate = 2048k
center = 14.2M
realtime = true
max_bandwidth = 12k
history = public
history_hours = 1
history_bins = 512
history_interval = 1

[band:40m]
name = 40 m
source = test
sample_rate = 192k
center = 7.1M
realtime = true
```

```json
{
  "widgets": [
    { "type": "chat", "title": "Chat", "height": 200 },
    { "type": "clock", "title": "Time" },
    { "type": "notice", "title": "About", "text": "A test receiver for the behaviour check." },
    { "type": "links", "title": "Links", "items": [{ "label": "Band plan", "url": "https://www.iaru-r1.org/" }] },
    { "type": "spots", "title": "Bands" }
  ]
}
```

It writes what each scenario saw and every command the page sent to
`report.json`; two builds of the same behaviour give the same report apart
from the clock and the stored display levels.

## Load and links

Capacity is measured with real protocol clients against a receiver that
allows them (`max_users = 1100`, `max_users_per_address = 0`,
`max_connections = 1200`). Point these at your own receiver only: the
protocol check sends chat messages.

```sh
python3 tools/protocol-check.py --port 18073
python3 tools/loadtest.py --port 18073 --clients 1000 --seconds 60 --ramp 10 \
  --width 1024 --fps 10 --codec wfc4 --audio-codec nac3 \
  --meter-format binary --modes usb,lsb,cw,cwl,am,sam,nfm,dsb
```

`make -C server link-model` runs one listener's stream budget over many link
shapes and stalls without a network. `tools/link-lab.sh`, as root with
`sch_netem` and `sch_plug` loaded, plays the same kinds of link to the real
page over the kernel's TCP.

Audio quality has its own laboratories: `make -C server quality` for the
codecs against their reference, `make -C server lab` for off-air recordings
through the receive chain, and `make -C server agc-lab` for the gain control
on synthetic scenes with a known answer.

## Not covered here

Physical ARM boards and older x86 machines, real RTL-SDR, SDRplay and RX-888
hardware, cellular links and a deployment running for weeks are tested by
operators, not in this suite. No independent security review has been done.
