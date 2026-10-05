# Modules

A module is a separate program that FernSDR starts and talks to. The receiver
itself links only the C and C++ runtime; everything that needs a vendor
library, USB access or a driver lives in a module. An input module delivers
samples for one band; a decoder module takes a narrow channel and reports
what it decodes. These are the ones in the default catalog:

| Module | Kind | For | Platforms |
|---|---|---|---|
| [Fern-RTLSDR](https://github.com/Steven9101/Fern-RTLSDR) | input | RTL2832U dongles, the RTL-SDR Blog V4 included | x86_64, aarch64, armhf |
| [Fern-RX888](https://github.com/Steven9101/Fern-RX888) | input | the RX-888 MkII sampling HF directly, 0 to 30 or 0 to 60 MHz at once | x86_64, aarch64, armhf |
| [Fern-SDRPlay](https://github.com/Steven9101/Fern-SDRPlay) | input | SDRplay RSPs, through SDRplay's own API | x86_64 |
| [Fern-Airspy](https://github.com/Steven9101/Fern-Airspy) | input | Airspy R2, Mini, HF+ and HF+ Discovery | x86_64, aarch64, armhf |
| [Fern-FT8](https://github.com/Steven9101/Fern-FT8) | decoder | FT8 | x86_64, aarch64, armhf |

The Airspy module has not been run with a real radio yet; the RX-888 module has had its first runs on real boards.

In plain words: FernSDR never talks to radio hardware itself. A module does,
a small program of its own for one kind of hardware, which FernSDR starts,
hands the band's settings to and takes samples from. This page is the
contract between the two; anyone can write a module against it, in any
language.

## Running a module

FernSDR starts the executable with exactly one argument list and a small,
fixed environment:

```
<module> --fernsdr-module 1
```

The environment holds `PATH=/usr/local/bin:/usr/bin:/bin`, `LANG=C.UTF-8`
and `FERNSDR_MODULE_API=1`, nothing else. There is no shell. The module runs
in a process group of its own, as the receiver's own user, inside whatever
sandbox the receiver runs in. The systemd unit that `install.sh` writes
forbids memory that is both writable and executable and limits system calls
to systemd's `@system-service` set, so a module that needs a JIT will not
run there.

Four file descriptors are open; every other one is closed:

| fd | direction | content |
|---|---|---|
| 0 | FernSDR to module | commands, one JSON object per line |
| 1 | module to FernSDR | samples, raw, in the format `ready` announced |
| 2 | module to FernSDR | human-readable log lines |
| 3 | module to FernSDR | events, one JSON object per line |

A module must exit, with status 0, when fd 0 reaches end of file or when a
write to fd 1 fails with `EPIPE`. That is how it learns FernSDR has gone, including when
FernSDR crashed: a module left running would keep holding the USB device.
`SIGPIPE` is at its default disposition when the module starts.

Lines on fd 0 and fd 3 are at most 64 KiB. Log lines on fd 2 are at most
1 KiB each; FernSDR keeps the last 200 of them for the admin panel and puts
at most 10 a second into its own log.

Both sides ignore message types and fields they do not know, so either can
add to the protocol without breaking the other. Settings are the exception:
see `open`.

A sample is one I/Q pair for a complex signal and one value for a real one.

## A session

```
module:   {"type":"hello","api":1,"id":"rtlsdr","version":"0.1.0","kind":"input"}
FernSDR:  {"type":"open","sample_rate":2400000,"center":14200000,"signal":"iq",
           "settings":{"device":"serial:00000001","gain":"auto"}}
module:   {"type":"ready","format":"u8","signal":"iq","sample_rate":2400000,"center":14200000,
           "device":{"name":"RTL-SDR Blog V4","serial":"00000001","tuner":"R828D"},
           "settings":{"gain":"auto","bias_tee":false}}
module:   samples on fd 1, continuously
module:   {"type":"stats","samples":24000000,"dropped":0}            about once a second
FernSDR:  {"type":"set","id":1,"settings":{"gain":38.6}}
module:   {"type":"applied","id":1,"settings":{"gain":38.6}}
FernSDR:  {"type":"stop"}          and closes fd 0
module:   exits with status 0
```

`hello` must arrive within 5 seconds of the start, and `ready` within 15
seconds of `open`. Before `ready` a module writes nothing to fd 1. `id` in
`hello` is the module's own id, the same as in its package.

### open

`sample_rate` and `center` are the band's, in Hz, and `signal` is `iq` or
`real`. `settings` holds every key the operator wrote in the band's section
as `module.<key>`, with the prefix removed.

FernSDR types each value by the settings the module declares in its package
(see Packages): a `string` or `choice` setting arrives exactly as written, a
`number` as a JSON number and a `boolean` as `true` or `false`. It refuses to
start a band whose section names a setting the module does not declare, gives
a number that is not one, or a choice that is not offered, and tells the
operator why. The keys `sample_rate`, `center` and `signal` are the band's own
and cannot be module settings.

A module applies what it can and reports the result. It must refuse, with an
`error`, a setting it does not know or cannot honour. It must not guess: a
receiver that says it is tuned to one frequency while it is tuned to another
misleads every listener.

### ready

| field | meaning |
|---|---|
| `format` | `u8`, `s8`, `u16`, `s16` or `f32`, little-endian where it matters |
| `signal` | `iq` for interleaved complex samples, `real` otherwise |
| `sample_rate` | the rate the hardware actually runs at, Hz; it need not be a whole number |
| `center` | the frequency tuned, Hz, as far as the module can tell and as finely as the hardware sets it; 0 for a real input sampled from DC |
| `device` | free-form description, shown to the operator |
| `settings` | the effective values, which may differ from those requested |

FernSDR builds its transforms for the band before the module runs, from the
band's own `sample_rate`, `center` and `signal`. It stops a module whose
`signal` differs, whose `sample_rate` differs by more than 100 parts per
million, or whose `center` differs by more than 10 Hz, and says why. Within
that the band keeps its axis, and the admin panel shows the centre the module
reported. Hardware that tunes in coarser steps shifts the difference out of
the samples itself before it writes them, or moves it where it can be set
finely: the RTL-SDR module works out where librtlsdr's settings put an R820T
or R828D, typically a few hundred hertz beside the frequency asked for, and
has the RTL2832U's mixer take that out, which leaves at most half its 6.9 Hz
step. What no module can know, a crystal's own error, the operator measures
and corrects with the band's `ppm` or `frequency_offset`. Changing a band's
rate or centre means restarting the band.

### set and applied

Only settings a module declares as live may change while it runs. Their
values are typed as in `open`. `id` is chosen by FernSDR; `applied`, with the
effective values, or an `error` with a `fatal` of `false`, answers every `set`
and carries the same `id`.

### stats

`samples` counts samples delivered since `ready`; `dropped` counts samples the
module lost, for example because the USB transfers overran. Both only grow.

Two more are for a module that can tell. `clipping` is the share of the
samples that arrived since the last stats which sat at the limit of the
converter, lost to every listener and splattered over the band. The admin
panel shows it, and while more than one sample in a thousand clips at a gain
the module does not set itself, the log says to lower the gain or put an
attenuator in front. `gain` is the gain in dB a module that sets the gain
itself is using now; such a module says in its own log when even its lowest
gain clips.

A module that has sent `ready` and then delivers no samples for 2 seconds is
treated as failed and stopped.

### error

```json
{"type":"error","code":"no-device","message":"no RTL-SDR with serial 00000001 is plugged in","fatal":true}
```

`code` is one of `no-device`, `busy`, `usb`, `lost`, `invalid` or
`internal`. A fatal error is followed by the module exiting. FernSDR shows
the message to the operator as it stands, so it should say what to do.

### Exit status

| status | meaning |
|---|---|
| 0 | stopped as asked |
| 2 | the command line was wrong |
| 3 | no device matched the settings |
| 4 | the device is in use by another program |
| 5 | USB failed or the device was unplugged |
| 6 | the settings were invalid |
| anything else | an internal failure |

FernSDR restarts an input module that exits or fails while its band is
running, after 1, 2, 4, 8, 16 and then every 30 seconds, and lists the band
as offline in the meantime. The delay starts again at 1 second once a module
has streamed for a minute. Statuses 2 and 6, and a band whose module is not
installed or is disabled, wait for the operator to change something.

### Stopping

FernSDR sends `stop`, closes fd 0 and its end of fd 1, then waits two seconds
for the module to exit, sends `SIGTERM` to its process group, waits two more,
and sends `SIGKILL`. It always collects the exit status.

## Asking a module about itself

Two further invocations run with fd 0 on `/dev/null`, without fd 3, and exit
when done:

```
<module> --describe
<module> --list-devices
```

`--describe` prints one JSON object describing the module and the settings
it takes:

```json
{"api":1,"id":"rtlsdr","name":"RTL-SDR","version":"0.1.0","kind":"input",
 "settings":[
   {"key":"device","type":"string","label":"Device","help":"serial:<serial>, index:<n>, or empty for the only one"},
   {"key":"gain","type":"string","label":"Gain","default":"auto","live":true,"help":"auto, or dB"},
   {"key":"bias_tee","type":"boolean","label":"Bias tee","default":false,"live":true}
 ]}
```

An input module can also say what its radio tunes, so the admin panel's
setup can offer bands that fit rather than ask for a centre frequency and a
sample rate:

```json
"tuning":{"ranges":[[500000,1766000000]],"rates":[2400000,2048000,1024000],"signal":"iq"}
```

`ranges` are up to 8 `[low, high]` pairs of centre frequencies the radio
tunes, and `rates` up to 8 sample rates worth offering, the first the
default, all in whole Hz; `signal` is `iq`, or `real` for a radio that
samples from 0 Hz up, as the RX-888 does. Without `tuning` the setup says it
cannot suggest bands for the radio, and the operator writes the band by hand.

A band written by hand for a module with `tuning` may leave `signal` and
`sample_rate` out: it takes the module's signal and first rate. One that
contradicts them (IQ from a real radio, or a centre other than 0 on a real
signal) is refused when the panel saves the file, with what to change; a
receiver that starts with such a file starts that band as before, for the
module to refuse. `ranges` only guide the suggestions: a band outside them,
such as long wave by direct sampling, is the module's to take or refuse.

`type` is `string`, `number`, `boolean` or `choice`, with `choices` for the
last and optional `min`, `max` and `unit` for numbers. `advanced: true` marks
a setting most operators should leave alone; the admin panel shows it apart
from the rest. The same `settings`
list goes into the package manifest, which is where FernSDR reads it: a
package is never run to find out what it accepts.

A setting whose value is a path on the machine (it starts with `/` or `~`,
or holds `..`), such as a firmware image, can only be set in the configuration
file on the machine, not from the admin panel: a module runs as the
receiver's user, and a stolen panel session must not choose what it opens.

`--list-devices` prints the devices it can see, and an empty list when there
are none:

```json
{"devices":[{"index":0,"name":"RTL-SDR Blog V4","serial":"00000001","tuner":"R828D","usable":true}]}
```

A device another program holds is listed with `"usable":false` and an
`error`. Both must finish within 10 seconds, and print at most 64 KiB.

## Packages

A module is installed from a `.fernmod` file: one executable with its
description, in a format that needs no archive tools to unpack.

```
FERNMOD1\n
<length of the manifest in bytes, decimal>\n
<manifest, UTF-8 JSON>
<the executable, exactly `size` bytes>
```

```json
{"schema":1,"id":"rtlsdr","name":"RTL-SDR","version":"0.1.0","kind":"input","api":1,
 "platform":"linux-x86_64","size":812345,"sha256":"<hex of the executable>",
 "license":"GPL-2.0-or-later","source":"https://github.com/Steven9101/Fern-RTLSDR",
 "description":"RTL2832U dongles, including the RTL-SDR Blog V4",
 "settings":[{"key":"device","type":"string","label":"Device"}]}
```

`id` is 1 to 32 lowercase letters, digits and dashes, starting with a letter.
`version` is `major.minor.patch`, each at most four digits. `platform` is
`linux-` followed by `x86_64`, `aarch64` or `armhf`; `armhf` means ARMv7 with
hardware floating point, which is every Raspberry Pi from the Pi 2 on, and not
the Pi Zero or Pi 1. `settings` and `tuning` are what `--describe` prints;
`name` is at most 64 characters, `license` 100, `source` and `description`
300, and there are at most 64 settings. The manifest
is at most 16 KiB and the executable at most 32 MiB. Executables should be
linked statically, so that one package runs on every distribution of its
architecture.

A module that drives hardware through a vendor's library which only the
vendor may hand out, such as SDRplay's API, cannot carry it. Such a module is
a dynamic executable against an old glibc, so it runs on the distributions
FernSDR supports, loads the vendor's library with `dlopen` when a band
starts, and says in `requires` what the operator installs first:

```json
"requires":["SDRplay API 3.14 or 3.15 for Linux from sdrplay.com, with its sdrplay service running"]
```

`requires` is optional: up to 4 lines of up to 200 characters, shown on the
admin panel's Modules page. Receivers from before it ignore the key.

A decoder module that decodes more than one mode lists them in `modes`, as its
`hello` does:

```json
"modes":["ft8","ft4"]
```

Each is a name a `[decoder:<id>]` section's `mode` may take, up to 8 lowercase
letters and digits. The Decoders page offers a choice among them for a new
decoder, and FT4 by itself once FT8 runs and the module lists it. Without
`modes`, a decoder is taken to decode the one mode its id names.

A module release on GitHub carries one `.fernmod` per platform, named
`<id>-<version>-<platform>.fernmod`. FernSDR refuses a package whose manifest
does not match the name it was published under.

### Where modules live

Installed modules live beside the configuration, in `fernsdr-modules/`
unless `[modules] directory` says otherwise:

```
fernsdr-modules/<id>/<version>/manifest.json
fernsdr-modules/<id>/<version>/module          the executable
fernsdr-modules/<id>/active                    the version bands use
fernsdr-modules/<id>/origin                    the repository it came from
fernsdr-modules/<id>/disabled                  present while the operator has it switched off
```

A version directory never changes once written. Installing writes to a
staging directory, checks size and SHA-256, and renames it into place;
activating rewrites `active` by rename. The version a running band uses
cannot be removed.

### Where they come from

`[modules] catalog` lists the GitHub repositories the admin panel may install
from, `owner/name` separated by spaces; it defaults to
`Steven9101/Fern-RTLSDR Steven9101/Fern-RX888 Steven9101/Fern-SDRPlay Steven9101/Fern-Airspy Steven9101/Fern-FT8`. FernSDR asks the GitHub API for their releases and
downloads the package for its platform with `curl` over HTTPS, with fixed
arguments and limits on time and size. The
URL is put together from the configured repository and the release's
validated tag and file name. Nothing a browser sends can name a URL. Only
curl is used, because it can be held to HTTPS through every redirect; wget
cannot outside recursive mode. A module id installed from one repository
cannot be replaced from another; remove it first. A module installed from a
file may later be updated from the catalog, and the repository it then comes
from becomes its origin.

An operator with a shell on the machine installs a package file directly:

```
sudo -u fernsdr fernsdr --install-module rtlsdr-0.1.0-linux-aarch64.fernmod fernsdr.conf
```

It refuses to run as a user other than the one that owns the modules
directory, so the files stay the receiver's to update and remove.

The `[modules]` section can only be changed in the file on the machine,
never through the admin panel.

### Trust

A module is a native program running as the receiver's own user, with the
same access to its configuration and devices. The service `install.sh` sets
up, like `tools/source-install.sh --service --usb`, widens the systemd sandbox
for modules: the host's `/dev` becomes visible, with access limited to USB
devices by `DevicePolicy=closed`; `/dev/shm` is shared with the host;
netlink sockets of every protocol are allowed, because libusb needs one
to see a dongle arrive; and USB devices other than RTL2832U dongles and the
RX-888's Cypress FX3 are limited only by their own file permissions. The SHA-256 in a manifest
proves the executable arrived intact; it does not prove who built it.
Installing from the catalog trusts GitHub and the account that published the
release. That is why the catalog is fixed in the configuration file and why
nothing installs or updates without an operator asking.

## Decoders

A decoder module receives narrow channels cut from a band and reports what it
decodes in them: FT8 today, and the same contract serves WSPR, FT4 or APRS.
In plain words: the receiver keeps doing the radio part, one shared transform
per band, and hands a decoder only the few kilohertz it listens to, with the
time each sample arrived; the decoder hands back what it heard, as data the
receiver checks before anyone sees it.

This is API 2. It is used only by modules of kind `decoder`; input modules
stay on API 1.

### Running a decoder

```
<module> --fernsdr-module 2
```

The environment is as for input modules, with `FERNSDR_MODULE_API=2`. Five
file descriptors are open:

| fd | direction | content |
|---|---|---|
| 0 | FernSDR to module | commands, one JSON object per line |
| 1 | nowhere | `/dev/null`; a decoder writes no samples |
| 2 | module to FernSDR | human-readable log lines |
| 3 | module to FernSDR | events, one JSON object per line |
| 4 | FernSDR to module | sample frames, binary, see below |

A decoder parses signals that anyone with a transmitter can send, so its
launcher sets `PR_SET_NO_NEW_PRIVS` and installs a seccomp BPF filter before
executing it. If either fails, the decoder does not run. The filter checks
the syscall architecture on x86-64, aarch64 and armhf and refuses creation
of sockets and socket pairs outside `AF_UNIX` with `EPERM`, including TCP,
UDP, netlink and packet sockets. io_uring setup and legacy ARM socket
multiplexing are refused so they cannot bypass this restriction.

`kill`, `tgkill`, `rt_sigqueueinfo` and `rt_tgsigqueueinfo` may target only
the original decoder process, including its threads. Process-group targets
are refused. `tkill`, `pidfd_send_signal`, `ptrace` and `pidfd_getfd` are
refused because their target process cannot be checked by this filter.
Forked helpers inherit the filter with the original decoder's PID.

Where Landlock is available and its rules apply, file access is limited to
the decoder executable, the system library directories (`/lib`, `/lib64`,
`/usr/lib`, `/usr/lib64`) and the loader cache (`/etc/ld.so.cache`), read
only, with execution allowed. Other file opens and device access are
denied. ABI 4+ also denies TCP bind/connect; ABI 6+ scopes signals and
abstract UNIX sockets to the decoder's Landlock domain. Without Landlock,
or if applying it fails, the decoder still runs with seccomp, and file
access remains subject to the receiver's existing OS permissions.

UNIX sockets remain allowed. They can reach local services and pass open
descriptors; seccomp does not inspect those descriptors or block network
operations on a socket received this way. This is not a complete isolation
boundary for hostile native code. Reporting spots, where enabled, is done
by the receiver. The decoder starts at nice 19 and `SCHED_IDLE` where the
OS permits those priorities.

The Decoders page shows the launcher's actual confinement report for the
most recent launch attempt: seccomp, applied file restrictions, Landlock
ABI and any failure. The report
travels through a separate pipe that is closed before the module executes;
module log and event messages cannot replace it.

A decoder exits with status 0 when fd 0 or fd 4 reaches end of file.

### A decoder session

```
module:   {"type":"hello","api":2,"id":"ft8","version":"0.1.0","kind":"decoder","modes":["ft8"]}
FernSDR:  {"type":"open","channels":[{"id":"20m-ft8","band":"20m","mode":"ft8","dial":14074000,
            "rate":8000,"offset":2000,"width":4000,"format":"cf32"}],
           "settings":{"depth":2}}
module:   {"type":"ready"}
FernSDR:  sample frames on fd 4, continuously
module:   {"type":"decode","channel":"20m-ft8","time":1790604000000,"freq":1234.5,"snr":-12,
           "dt":0.3,"message":"CQ DL1ABC JO31","call":"DL1ABC","grid":"JO31","quality":"bp"}
module:   {"type":"stats","channels":[{"id":"20m-ft8","slots":40,"decodes":312,"late":0,"cpu_ms":58}]}
FernSDR:  {"type":"stop"}          and closes fd 0 and fd 4
```

`hello` must arrive within 5 seconds and `ready` within 15 seconds of
`open`.

In `open`, each channel is complex baseband at `rate` samples a second, which
need not be a whole number, centred `offset` hertz above the `dial` frequency
and `width` hertz wide: with the values above, baseband 0 Hz is the audio
frequency 2000 Hz of a USB receiver set to the dial, and the channel covers
0 to 4000 Hz of that audio. `format` is `cf32`, pairs of little-endian 32-bit
floats. `settings` holds the decoder section's `module.<key>` values, typed
as for input modules.

### Sample frames

Each frame on fd 4 is a 32-byte header, little-endian, then its samples:

| offset | size | field |
|---|---|---|
| 0 | 4 | magic, the bytes `FDR1` |
| 4 | 2 | channel, its index in `open`'s list |
| 6 | 2 | flags: 1 = samples were lost before this frame; 2 = the clock was set again, for example after the band restarted |
| 8 | 4 | count of samples that follow, at most 65 536 |
| 12 | 4 | zero |
| 16 | 8 | index of the frame's first sample, counting from 0 at `open`, per channel |
| 24 | 8 | UTC time of the frame's first sample, microseconds since 1970, as a signed integer |

A frame's samples are contiguous in time; a gap is only ever before a frame,
and flagged. The index keeps counting over a gap, so the decoder can tell how
much is missing. The time comes from the band's own sample count, anchored to
the host's clock by the earliest arrival seen, and it moves smoothly: it does
not jump with the jitter of the input. Timing is only as good as the host's
clock, which must be kept by NTP for anything reported to others.

FernSDR never waits for a decoder. When fd 4 is full it drops whole frames
and sets flag 1 on the next one it can write, so a slow decoder loses slots
rather than delaying the band.

### Decodes

A `decode` names its `channel`, the UTC start of its slot in milliseconds as
`time`, the audio frequency above the dial in hertz as `freq`, `snr` in dB,
`dt` in seconds, and the `message` as the mode prints it. Optional fields say
what the message holds: `call` (the transmitting station), `grid` (a
Maidenhead locator of 4 or 6 characters) and `report`. `quality` is `bp`, `osd`
or `low`: how the decode was made, so that only confident ones are reported
to others.

FernSDR checks every field before it keeps a decode: the channel must be one
it opened, `message` at most 64 printable ASCII characters, `call` at most
12 of `A-Z 0-9 /`, `grid` a real locator, `freq` inside the channel, `snr`
between -60 and 60 and `dt` between -5 and 5. A decode that fails is dropped
and counted, and each channel may report at most 200 decodes a slot. Nothing a
decoder sends reaches a page as markup.

`stats`, at most once a second, carries per channel the `slots` decoded, the
`decodes` made, the slots that came in `late` and the CPU time spent in
milliseconds. `error` is as for input modules.

### Configuring a decoder

A decoder runs from a section of its own, naming the module and the channels,
each as `<band>:<dial in Hz>`:

```ini
[decoder:ft8]
module = ft8
channels = 40m:7074000 20m:14074000
module.depth = 2
public = yes
```

`width` (default 4000 Hz) and `offset` (default half the width) place each
channel above its dial. `public = yes` shows the decodes to listeners, on the
page and through `/api/decodes`; it is off unless set, as the decodes name
other stations and their locators.

`report = pskreporter` reports what the decoder hears to
[PSK Reporter](https://pskreporter.info), under the callsign and locator on
the Station page (`[site] operator` and `grid`), with the antenna when one is
given. FernSDR sends, not the module: a datagram every
five minutes or so, not in step with the clock, each station at most once an
hour on each band unless its locator changed, and only confident decodes of stations that
sent their call in full. The Decoders page has it as a switch and says what
went out and when, or what is missing. `[site] spot_server` changes where
spots go, `host:port`; PSK Reporter's test port is 14739. Saved from the admin panel or its
configuration editor, a changed section applies at once and restarts only
that decoder; one whose channels need a band that only a restart brings up
waits for that restart.

The admin panel writes the same section. FernSDR restarts a decoder that
exits after 1, 2, 4, 8, 16 and then every 30 seconds, and a band that
restarts sets flag 2 on its channels' next frames.
