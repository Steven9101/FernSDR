# FernSDR

A radio receiver you use through a web browser, and the server that runs it.

Point a radio at an antenna, run this next to it, and anyone with the link can
listen. They pick a frequency, they hear what the antenna hears, and they see a
picture of the radio spectrum scrolling past. Listeners tune independently;
the server's CPU and uplink determine how many can listen at once.

## If none of that meant anything

Radio signals arrive at an antenna all mixed together, across a wide range of
frequencies, the way every station in a city arrives at a car aerial at once. A
receiver picks one out. This one does it in software and puts the controls on a
web page, so the radio can sit somewhere with a good antenna and quiet
surroundings while you listen from anywhere.

Three words appear throughout and are worth knowing:

- A **band** is a stretch of frequencies the radio is looking at, the way a
  camera has a field of view. 20 m is one, 40 m is another.
- The **waterfall** is the scrolling picture. Left to right is frequency, top
  to bottom is time, and brightness is how strong a signal is. Vertical lines
  are stations transmitting.
- A **passband** is how wide a slice you listen to. Narrow it to shut out the
  station next door; widen it for better audio when nobody is crowding you.

## Install it

On any Linux, on x86_64 or ARM (a Raspberry Pi 2 or later):

```sh
curl -fsSL https://github.com/Steven9101/FernSDR/releases/latest/download/install.sh | sudo sh
```

It asks whether the receiver runs at home or on a server on the internet,
installs it as a service, and prints its address and a password for the admin
panel. Until an antenna is connected it receives a synthetic band, so it works
in a browser first. Run again, it updates to the newest release, as the admin
panel's Updates page does. [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md#installing)
says what goes where, and how to run it in Docker instead, and
[docs/GUIDE.md](docs/GUIDE.md) goes from the
hardware to the first listener step by step, a Raspberry Pi and an RTL-SDR
included.

## Build it yourself

Use Linux with a C++17 compiler and Make. Building the browser files also
needs Node.js 20.19+ or 22.12+ and npm. On Debian or Ubuntu, install the server
build tools with `sudo apt-get install build-essential`.

```sh
tools/source-install.sh
```

The script builds and tests the server, builds the page, writes
`server/fernsdr.conf` if it does not exist, and starts a synthetic radio.
Open <http://localhost:8073> and press **Start audio**. Stop it with Ctrl-C.
No antenna or radio driver is needed for this check.

For another port, use `tools/source-install.sh --port 8090`. To build without
starting, use `tools/source-install.sh --no-start`. Existing settings are kept when you rerun it.
Small machines automatically use fewer compiler jobs. Set `FERNSDR_JOBS=1`
before the command to force a single job if memory is tight.

Node is a build tool, not a server requirement. You can build `web/dist` on
another machine and copy it over. The backend links only the system C/C++
runtime, math and threading libraries. FFTs use SSE2 on x86 and NEON on ARM;
the same x86 binary selects AVX2, and on AMD AVX-512, when the machine
supports them. Other targets retain a scalar path. No machine-specific build flags are needed.

## With a real radio

Hardware is attached through a pipe rather than a driver, so anything that can
write samples to standard output works and the server never needs a vendor SDK:

```sh
rtl_sdr -f 14200000 -s 2048000 - | server/build/fernsdr server/fernsdr.conf
```

Set `source = stdin`, along with the sample rate and centre frequency you gave
the radio, in the matching `[band:...]` section. An RTL-SDR dongle can instead
be run by the receiver itself through the RTL-SDR module, installed from the
admin panel, which restarts it when it is unplugged and plugged back in; see
[docs/MODULES.md](docs/MODULES.md).
[docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) covers RTL dongles, the RX-888,
ka9q-radio over multicast, several bands from one radio, replaying a recording,
and putting HTTPS in front.

## What it does

- **SSB, CW, AM, SAM, NFM and DSB**, the ways a signal can be encoded, with a
  passband you can drag to any width
- **A waterfall and spectrum** with band-plan markers, peak hold and four
  colour maps
- **Waterfall history up to 24 hours**, when the operator enables disk recording
- **Noise reduction, an automatic notch and manual notches**, all off unless
  you turn them on
- **Automatic gain control** in four speeds, or set the gain yourself
- **An automatic squelch** that mutes when the passband is only noise, working
  from the shape of what it sees rather than a level, so it needs no threshold
- **A signal meter** that reads real dBm once the operator has calibrated it,
  and says plainly that it is relative until then
- **Several bands at once**, with a scrollable band list on desktop and mobile
- **A remembered frequency, filter and zoom for each band**, including after a reload
- **Links that open on the same signal and zoom**, including the beat-note pitch for CW
- **A chat where frequencies are links**, so "14074 FT8" is one tap away from
  being tuned in, and one button puts where you are listening into the box
- **An admin panel** at `/admin`, as usable on a phone as on a desktop: each
  band live with its spectrum and the last minute of waterfall, who is
  listening, band and station settings, the receiver's look and widgets,
  hardware modules, the log and the configuration file; a mute for the chat
  that outlasts a page reload, and `/metrics` for whoever watches the machine

Drag the tuned marker or the shaded filter to tune. Drag either filter edge
to change its cutoff, including over the waterfall. Drag the background to
pan; Shift-drag always pans. Scroll or pinch to zoom around the pointer.
Filter edges mark the transition, not a brick wall: a strong signal just
outside a cutoff can still be audible.

## Running one for other people

The admin panel lives at `/admin`. `install.sh` makes up its password; for a
build of your own, set one with

```sh
server/build/fernsdr --hash-password
```

and put the result in the `[admin]` section of `server/fernsdr.conf`. Remote
administration requires HTTPS; plain HTTP administration is limited to the
machine itself, which an SSH tunnel can reach, and with `home_network = yes`
to the home network. [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) covers HTTPS and running as
a service.

## How it is built

One server binary and a browser client with three runtime dependencies. The
audio codec, waterfall codec, DSP and HTTP server live in this repository.
Each band shares its large FFT across listeners. Each listener has independent
tuning, audio processing and a bandwidth budget. Congestion reduces waterfall
traffic first, then audio bitrate if the queue continues to grow.

[docs/PERFORMANCE.md](docs/PERFORMANCE.md) has the measurements and
[docs/TESTING.md](docs/TESTING.md) how everything is checked. Capacity depends on band width, listening modes, hardware and network
capacity. A jitter buffer can cover short delivery gaps; it cannot deliver
live audio while a link is disconnected or indefinitely below the stream rate.

[docs/PRINCIPLES.md](docs/PRINCIPLES.md) says how everything here is built,
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) explains the shape of it,
[docs/CODEC.md](docs/CODEC.md) the audio and waterfall codecs,
[docs/PERFORMANCE.md](docs/PERFORMANCE.md) how those numbers were taken,
[docs/PROTOCOL.md](docs/PROTOCOL.md) the wire format,
and [docs/DESIGN.md](docs/DESIGN.md) the interface.

## Tests

```sh
make -C server test    # the DSP, the codecs, the server, the admin login
cd web && npm test     # the client
```

## Licence

See [LICENSE](LICENSE).
