# Deployment

Everything about running a receiver: installing and updating it, feeding it
from a radio, putting it behind HTTPS, the admin panel, and what to do when
something is wrong. It is the reference; [GUIDE.md](GUIDE.md) walks through
a first receiver step by step.

## Installing

On a Linux machine with an x86_64 or 64-bit ARM processor, or a 32-bit ARMv7
one with NEON such as a Raspberry Pi 2 or later, whatever the distribution:

```sh
curl -fsSL https://github.com/Steven9101/FernSDR/releases/latest/download/install.sh | sudo sh
```

`wget -qO-` in place of `curl -fsSL` does the same. The installer asks one
question, where the receiver runs:

1. **At home.** It listens on every address, port 8073, and the admin panel
   takes plain HTTP from the home network (`home_network = yes`, see
   [Administration without HTTPS](#administration-without-https)).
2. **On a server with a domain name**, answered as `2 radio.example.org`. It
   listens on 127.0.0.1 only, and where the distribution packages Caddy and
   the init is systemd, installs Caddy and writes its site into
   `/etc/caddy/Caddyfile` between marker lines, keeping the file before as
   `Caddyfile.before-fernsdr`; Caddy then fetches the certificate. Elsewhere
   it prints the lines that make Caddy, or another web server, do it.
3. **On a server without a domain name.** It serves listeners plain HTTP on
   port 8073, and the admin panel only through an SSH tunnel
   (`ssh -L 8073:localhost:8073 you@server`, then
   `http://localhost:8073/admin`), unless you answer yes to the admin panel
   over plain HTTP from anywhere, at your own risk (`plain_http_anywhere`).

Without a terminal to ask on, as from a provisioning script, `FERNSDR_SETUP`
gives the answer: `home`, `internet radio.example.org`, `internet` (only
listening locally, for a web server of your own), `http`, or `http admin`
for the third answer with the admin panel over plain HTTP.

Where Docker runs and nothing is installed yet, it first asks whether to
install the receiver as a service of the machine or in a Docker container;
`FERNSDR_DOCKER=1` answers the container, and `FERNSDR_IMAGE` names another
image than `ghcr.io/steven9101/fernsdr`, on every run, or the next one goes
back to that. See [In a container](#in-a-container).

It makes up a password for the admin panel and keeps it in
`/opt/fernsdr/admin-password`, which only root can read; the receiver has
only its hash. On a terminal it prints the password too, but not into the log
of a run without one. Until an antenna is connected the receiver runs a synthetic
band, so the whole chain can be tried in a browser first. The first sign-in
to the admin panel opens its setup, which finds a radio plugged in, installs
its module and adds its bands; the service may open USB devices, and nothing
else, and RTL2832U dongles belong to its user.

Run again on a machine where FernSDR is installed, the installer asks what
to do: `1` updates to the newest release, `2` makes a new admin password
when the old one is lost, prints it and restarts the receiver
(`FERNSDR_NEW_PASSWORD=1` asks for that without a terminal). A password
changed in the admin panel is only in the configuration, as a hash, so
`/opt/fernsdr/admin-password` then no longer holds it; a new one from the
installer does. On a receiver of your own,
`fernsdr --set-password fernsdr.conf` sets one from the terminal and keeps
the file's owner.

What goes where:

| Path | What |
|---|---|
| `/opt/fernsdr/releases/VERSION/` | each installed version: the program, the pages, the systemd units, the scripts for other inits and an example configuration |
| `/opt/fernsdr/current` | the version the service runs |
| `/opt/fernsdr/trusted` | the version known to work, whose program runs the next update |
| `/var/lib/fernsdr/` | the configuration, `fernsdr.conf`, and everything the receiver writes: settings saved in the panel, modules, the waterfall archive |
| `/var/lib/fernsdr-update/` | the updater's account of the last update, and its record of one under way |

The receiver runs as the user `fernsdr` from `fernsdr.service`, and can write
to `/var/lib/fernsdr` and, apart from a private `/tmp`, nowhere else: not to
its own program, and not to the updater's directory. The release's archive is
checked against the size and SHA-256 its manifest names, and the manifest's
signature is checked with OpenSSL 3 where that is installed; without it, the
first install relies on HTTPS, and every update after it is checked by the
receiver's own updater.

### Without systemd

Where the init is OpenRC (Alpine, Gentoo, Artix), runit (Void, Artix) or SysV
init (Devuan, MX Linux, Slackware), the installer puts a script of a few
lines in `/etc/init.d/fernsdr`, `/etc/sv/fernsdr/run` or
`/etc/rc.d/rc.fernsdr` that starts `fernsdr --supervise` at boot. That one
process, which runs as root, does what the systemd units do:

- Before anything else it ends an update that a restart of the machine
  interrupted.
- It runs the receiver as `fernsdr`, with root gone for good and no way back
  to it, and starts it again two seconds after it stops. After five starts
  within ten seconds it tries only every thirty seconds, until a start
  lasts.
- When the receiver stops, it stops whatever modules the receiver left
  running, so that the next one finds the radio free.
- When the admin panel asks for an update, it starts the updater from the
  trusted version and restarts the receiver when the updater asks.

What it lacks is the units' sandbox. There is no system call filter, and the
receiver can read what its user may read anywhere, not only
`/var/lib/fernsdr`. Programs it starts as root get the updater unit's
capabilities and no others.

Its log, and the receiver's and the updater's, is
`/var/log/fernsdr/fernsdr.log`, with the megabyte before in `fernsdr.log.1`.
To restart the receiver: `rc-service fernsdr restart`, `sv restart fernsdr`,
or `/etc/init.d/fernsdr restart` (`/etc/rc.d/rc.fernsdr restart` on
Slackware). On Alpine without eudev, the lines that give dongles and
RX-888s to the receiver's group go into `/etc/mdev.conf` in place of a udev
rule.

On a machine with none of these inits, such as a container that runs a
shell, the installer puts the same script in
`/usr/local/sbin/fernsdr-service`, starts FernSDR with it, and asks for
`/usr/local/sbin/fernsdr-service start` to be added to whatever the machine
runs at boot.

A receiver that `tools/source-install.sh --service` set up in `/opt/fernsdr`
moves over. With the old one stopped, its directory `/opt/fernsdr/etc` is
copied to `/var/lib/fernsdr`, and paths in the configuration that named it
name the new one. The old program, pages and directory go to
`/opt/fernsdr/old-layout` once the new receiver answers; remove that when it
does all the old one did. If the release refuses the configuration, or does
not start with it, the old receiver is put back as it was and the copy set
aside.

### In a container

The installer sets a container up too: where Docker runs and nothing is
installed yet, it asks whether to use one, and it updates it when run again
(see below). By hand,
each release publishes `ghcr.io/steven9101/fernsdr`, for x86_64, aarch64 and
armhf; `make -C server docker` builds one locally, 34 MB on Alpine.

```sh
docker run -d --name fernsdr --restart unless-stopped -p 8073:8073 \
    -v fernsdr:/var/lib/fernsdr ghcr.io/steven9101/fernsdr:latest
docker logs fernsdr        # the admin password, on the first start
```

On the first start with an empty volume it writes `fernsdr.conf` there,
with the synthetic band and a password for the admin panel, which the log
shows once and `admin-password` in the volume keeps. The receiver runs as
`fernsdr`, under tini, which collects whatever a module leaves behind. A
host directory works as the volume too, whoever owns it: the container
gives it to `fernsdr` when it starts.

- `-e FERNSDR_SETUP=internet` on the first start leaves the admin panel to
  HTTPS through a web server on the host, which reaches the container
  through a port published on the host's 127.0.0.1 only
  (`-p 127.0.0.1:8073:8073`). Docker hands those connections over from its
  bridge's gateway, not from loopback, so the configuration it writes
  believes the gateway's forwarded addresses as well
  (`trusted_proxies = loopback, 172.17.0.1`). The default, `home`, lets the
  home network in over plain HTTP. `http` is the installer's third answer:
  in a container the admin panel then answers only with
  `-e FERNSDR_PLAIN_ADMIN=1`, plain HTTP from anywhere at your own risk,
  because an SSH tunnel arrives from the gateway too and cannot be told
  from anyone else, so the installer asks for that or stops. Docker's usual
  port forwarding keeps each client's own address; where it does not, as
  with rootless Docker, every client arrives from a private address, so
  use `internet` there.
- `--device /dev/bus/usb/001/005` passes one USB device, an RTL-SDR or an
  RX-888; the container gives its node to the receiver's group. The numbers
  change when it is plugged in again. The installer instead binds all of
  `/dev/bus/usb` and allows USB devices by number
  (`--device-cgroup-rule 'c 189:* rmw'`), so a radio plugged in again later
  is found, and passes the host's group for radios as `FERNSDR_USB_GID`.
- Updates are a new image. Run the installer again: it pulls the image,
  keeps the one before as `:previous`, starts the container from the new one
  and goes back to the old one if the new does not answer. By hand,
  `docker pull` and start the container again from it. The Updates page
  says so; the volume keeps everything. Modules install from the admin panel
  as elsewhere and live in the volume.
- A module that loads a vendor's library, such as the SDRplay module, does
  not run in this image: the library needs glibc, which Alpine does not
  have, and a vendor service beside it. For an SDRplay RSP, install FernSDR
  on the host instead. The API talks to its service through shared memory in
  `/dev/shm`, which FernSDR's systemd unit shares with the host; a container
  of your own that holds the receiver needs the host's IPC namespace
  (`--ipc=host`) or the service inside it.

## Updates

The admin panel's Updates page says which version runs, looks for a newer
one when asked, and shows its notes. After signing in, the panel also asks
the receiver to look once a day, and offers a newer version in a dialog with
its notes until *Later* puts that version away; nothing is installed
without *Update*. The notes are the changelog's section for the version,
shown as text: headings, lists and `code`, never markup. *Update* asks the updater, which runs as
root beside the receiver, in `fernsdr-update.service` or started by the
supervisor, with the trusted version's program:

1. It downloads the release and checks it, the manifest's signature against
   the keys that program carries and the archive against the manifest,
   unpacks it beside the running version, and has the new program check the
   configuration as the receiver's user. A configuration the new version refuses ends the update
   there, with its reason on the page.
2. It keeps a copy of `fernsdr.conf`, `fernsdr-settings.json` and
   `fernsdr-theme.json`, switches `current` to the new version and restarts
   the receiver. Listeners drop out for a few seconds, and the panel's
   session ends with the old process.
3. Once the new version has served for a minute with every band that ran
   before running again, it says it works: the updater makes it `trusted`
   too and removes all but it and the version before.
4. If it does not say so within five minutes, or keeps stopping, the updater
   switches back, puts the three files back as they were and restarts the
   receiver. The page says why.

Every step is recorded before it is taken. A machine that goes down during an
update ends it at the next start, with one version or the other, before the
receiver starts: that is `fernsdr-update-boot.service`, or the supervisor's
first step. Nothing updates by
itself, and a receiver never moves to an older version.

Running `install.sh` again and answering `1` does the same from the command
line, and prints how it goes. It also brings the new version's systemd units, which an update
from the panel leaves alone. The configuration stays as it is, except that
one without a password for the admin panel gets one.

```sh
journalctl -u fernsdr-update -u fernsdr-update-boot   # what the updater did
cat /var/log/fernsdr/fernsdr.log                       # the same, without systemd
cat /var/lib/fernsdr-update/status.json                # how the last update ended
```

To go back to the version before by hand, while no update is under way:

```sh
cd /opt/fernsdr
sudo ln -sfn releases/0.1.0 current && sudo ln -sfn releases/0.1.0 trusted
sudo systemctl restart fernsdr     # or the init's restart command, above
```

Releases are fetched from GitHub. `FERNSDR_UPDATE_URL`, set for the units in
a systemd drop-in, or without systemd in `/etc/default/fernsdr` (on OpenRC,
`export FERNSDR_UPDATE_URL=...` in `/etc/conf.d/fernsdr`), points the Updates
page and the updater somewhere else: an
`https://` address ending in `/` that serves the same files. The signature
decides what is taken, wherever it came from.

## Moving to another machine

The Updates page's *Download* makes a backup: one JSON file with the
configuration, `fernsdr-settings.json`, `fernsdr-theme.json`, the pictures
the panel stored (up to 64 of them and 6 MB; the page says how many were
left out) and the
list of installed modules. It leaves out the `[server]`, `[modules]` and
`[admin]` sections, so it holds no password hash and signs nobody in.

On the new machine, install FernSDR, sign in and choose *Restore its
backup* in the first step of the setup, or *Restore a backup* on the Updates
page. The receiver then:

1. keeps its own `[server]`, `[modules]` and `[admin]` sections: its address,
   its module folder and its password;
2. drops the old machine's `history_path` and `theme_file` settings, so the
   new one keeps those files where it keeps them;
3. checks the result as the configuration editor checks a save. A band that
   reads a file (`path`) or takes samples over the network (`source = udp`)
   is refused with the reason, as it would be in the editor: set it up in the
   file on the new machine, or copy `fernsdr.conf` over by hand;
4. writes the files, each beside its place first and then renamed over it,
   so a full disk leaves the receiver as it was; installs the modules the
   backup names from the catalog; and restarts. Until the restart, the panel
   refuses other changes, which would be made to the old settings.

A receiver started by hand, not as a service, cannot restart itself: the page
says so, and the backup waits in the files for the next start. The waterfall
archive is not in the backup; copy the `.wfa` files by hand to keep it.

Two things stay with each machine and are not in the file: the id the
receiver reports to the public list under, which is what proves an entry is
its own, and the chat's mutes, which are listeners' addresses. The new
machine is listed as an entry of its own once its listing is on; switch the
old machine's listing off on its Station page.

## Building from source

```sh
make -C server              # produces server/build/fernsdr
make -C server test
cd web && npm ci && npm run build
```

The server runs on Linux and needs a C++17 compiler and Make to build. No libusb, no vendor
SDKs, no FFT library, no JSON library, no TLS library. On a Debian-family
system `build-essential` is the whole list.

The client needs Node 20.19+ on the 20.x line, or Node 22.12+ only to build. The output in `web/dist`
is static files. `tools/source-install.sh` runs these steps and starts a test
receiver. Use `sudo tools/source-install.sh --service` to install into
`/opt/fernsdr` and run under systemd as the `fernsdr` user. Its settings are in
`/opt/fernsdr/etc/fernsdr.conf`. `--prefix DIR` changes that location. Service
installs must be outside home directories because the service has
`ProtectHome=yes`, and cannot share a machine with one `install.sh` made,
whose service has the same name; the script refuses to replace it.

On a small machine, build `web/dist` elsewhere and copy the entire directory
before running `tools/source-install.sh --no-client`. This skips npm and installs the
prebuilt files, including their protocol marker. `FERNSDR_JOBS=1` limits
parallel C++ compilation when RAM is scarce. The script otherwise chooses
jobs from available CPUs and memory, capped at eight.

### Precompressing the client

`npm run build` creates `.gz` sidecars automatically. The server serves them
when the browser accepts gzip. No runtime compressor is needed.

The build also writes `licenses.txt` and `admin-licenses.txt` beside the
pages: the licence of every library and font each page includes, which the
MIT, ISC and OFL licences ask to travel with the code. The receiver's page
links the first.

## Feeding it

The receiver never talks to hardware itself. A radio with a module, the
RTL-SDR, the RX-888 and the SDRplay RSPs, is run by that module, a small
program the receiver starts for the band and restarts when the radio comes
back after being unplugged. Any other radio feeds it through a pipe from its
own program, and ka9q-radio or anything else that sends IQ over the network
through UDP.

The admin panel's setup does the usual case by itself: it finds the radio on
USB, installs its module from the catalog and writes the bands. The sections
below are what it writes, for doing it by hand and for everything else.

### RTL dongle

The RTL-SDR module works with RTL2832U dongles: the RTL-SDR Blog V3 and V4,
and sticks with an R820T, R828D, E4000, FC0012, FC0013 or FC2580 tuner.

1. Install the module: the setup does, or in the admin panel open Modules,
   press *Check for updates* and install `rtlsdr`. On a machine without
   internet access, copy the package for its platform from the module's
   releases and run
   `./fernsdr --install-module rtlsdr-0.1.0-linux-aarch64.fernmod fernsdr.conf`.
2. A receiver installed from a release by `install.sh` may open USB devices
   already. One built from source as a service needs this once:
   `sudo tools/source-install.sh --service --usb`. Either way the service may
   open USB devices and nothing else, RTL2832U dongles belong to the
   receiver's user, and the DVB-T television driver is kept off them
   (`/etc/udev/rules.d/61-fernsdr-usb.rules`,
   `/etc/modprobe.d/fernsdr-rtlsdr.conf`).
3. *Find devices* on the Modules page lists the dongles with their serials.
   Describe the band:

```ini
[band:20m]
source          = module
module          = rtlsdr
sample_rate     = 2400k
center          = 14.175M
module.device   = serial:00000001
module.gain     = auto
dc_remove       = yes
```

Every `module.` setting is checked against what the installed module
declares, so a typing mistake is refused when the file is saved rather than
when the band starts. The gain, the RTL2832's AGC and the bias tee change live
from the band's page. `module.direct_sampling` defaults to `auto`: off on the
Blog V4, whose upconverter covers HF, and the Q branch below the tuner's
lowest frequency on other sticks.

`module.gain = auto`, the default, is the module's own control: the highest
of the tuner's gains that keeps the 8-bit converter out of clipping with 6 dB
to spare. A signal that keeps clipping brings it down within a third of a
second, while a crash of static that clips for a moment does not; it goes
back up after a quiet spell, of seconds while the band starts and a minute
after that. The band's page shows the gain it chose and how much clips, and the
Overview flags a band that clips. `tuner` hands the gain to the tuner's own
AGC, which watches the tuner and not the converter, and a number such as
`38.6` fixes it. An S-meter calibration holds at the gain it was made at, so
a calibrated band wants a fixed gain.

`dc_remove = yes` takes out the spike in the middle of the waterfall.
The module puts the band within 3.4 Hz of where it was asked for, correcting
where the R820T's and R828D's synthesizer lands; a crystal without
temperature control still drifts a few parts per million. On the band's page
in the admin panel, "Measure against a known carrier" reads where a station
such as WWV or CHU shows and writes the correction into the configuration, as
`ppm` for the crystal's error or `frequency_offset` for a fixed one.

How modules are packaged, installed and trusted, and how to write one, is in
[MODULES.md](MODULES.md).

### RX-888 MkII

The RX-888 module samples the HF input directly, 0 to 30 MHz at 64.8 Msps
or 0 to 60 MHz, 6 m included, at 129.6 Msps, over USB 3. It carries the
board's firmware itself. Only the MkII is supported, and only its HF input;
the module has not been run with a real board yet.

```ini
[band:hf]
name          = Shortwave, 0 to 30 MHz
source        = module
module        = rx888
signal        = real
sample_rate   = 64800000
center        = 0
low           = 0
high          = 30M
module.gain   = auto
```

`module.gain = auto` sets the amplifier in front of the converter as the
RTL-SDR module does; `module.attenuation` (0 to 31.5 dB), `module.bias_tee`
and `module.dither` change live. The band is a real signal from 0 Hz up, so
`center` is 0 and `low` and `high` say what listeners see. A band this wide
is heavy on the shared transform, before any listener. `tools/wideband-check.sh`
measures it on the machine at hand; on a Ryzen 9 9950X held to AVX2, as an
older Intel Core has it, a band took 43 % of a core at 64.8 Msps and 74 % at
129.6 Msps, with no samples lost (a virtual machine of six cores, 20
seconds each). A core of a 6th-generation Core i5 such as the i5-6500T is
roughly a third as fast, which puts 0 to 30 MHz at about one and a third of
its four cores and 0 to 60 MHz at about two and a quarter: an estimate from
that ratio, not a measurement on one. See [PERFORMANCE.md](PERFORMANCE.md) before
choosing a user limit. ARM boards have not been measured with it.

### SDRplay RSP

The SDRplay module runs the RSP1, RSP1A, RSP1B, RSP2, RSPduo (one tuner at a
time), RSPdx and RSPdx-R2 through SDRplay's own API, which SDRplay does not
let anyone pass on. It is for x86_64 machines, and not for the container
image: the API needs glibc and SDRplay's service beside it. The module has
not been run with a real RSP yet.

1. Download the API for Linux, 3.14 or 3.15, from
   <https://www.sdrplay.com/api/> and install it:
   `sudo sh SDRplay_RSP_API-Linux-*.run`. It installs a service, `sdrplay`,
   which has to be running.
2. Install the module `sdrplay`, in the setup or on the Modules page. The
   Modules page shows what it needs first until the API is found.
3. For the first RSP, the RSP1: the kernel has a driver of its own for its
   chip, `msi2500`, which takes the radio as a TV tuner before SDRplay's
   service can. `install.sh` keeps it away
   (`/etc/modprobe.d/fernsdr-sdrplay.conf`) and unloads it when it is loaded;
   if the RSP1 was plugged in before, unplug it once or restart the machine.

```ini
[band:40m]
source          = module
module          = sdrplay
sample_rate     = 2000000
center          = 7.1M
module.device   = serial:1234567890
module.antenna  = auto
module.gain     = auto
```

`module.gain = auto` chooses the LNA state and IF gain itself, as high as
keeps the converter out of clipping; `manual` with `module.lna_state` and
`module.if_gain_reduction`, or the API's `agc`, instead. The notch filters
(`module.rf_notch`, `module.dab_notch`, `module.am_notch`), the bias tee and
`module.ppm` change live; `module.antenna` picks the input on the RSP2
(`a`, `b`, `hiz`), the RSPduo (`tuner1`, `tuner2`, `hiz`) and the RSPdx
(`a`, `b`, `c`). Rates of 2, 6 and 10 Msps are what the setup offers; any
from 62.5 kHz to 10.66 MHz works.

### Radios without a module

Every SDR with a program that writes its samples out can feed a band. Run in
a terminal, the program's output goes straight into the receiver:

```sh
rtl_sdr -f 7100000 -s 2048000 - | ./fernsdr fernsdr.conf
```

with the band reading standard input:

```ini
[band:40m]
source      = stdin
format      = cu8
sample_rate = 2048k
center      = 7.1M
```

`format` is how the program writes each sample: `cu8` (unsigned 8-bit IQ),
`cs8`, `cs16` or `cf32`, or for a real signal from 0 Hz up, with
`signal = real`, `u8`, `s8`, `s16` or `f32`. `sample_rate` and `center` are
what the program was told. Only one band can read standard input.

These are the usual programs, each with the `format` it writes:

| Radio | Command | `format` |
|---|---|---|
| RTL-SDR | `rtl_sdr -f 7100000 -s 2048000 -` | `cu8` |
| HackRF | `hackrf_transfer -r - -f 7100000 -s 2000000 -l 16 -g 20` | `cs8` |
| Airspy R2 or Mini (24 MHz and up) | `airspy_rx -r - -f 145.0 -a 2500000 -t 2` | `cs16` |
| Airspy HF+ | `airspyhf_rx -r stdout -f 7.1 -a 768000` | `cf32` |
| anything SoapySDR knows: LimeSDR, Pluto, Airspy HF+, SDRplay | `rx_sdr -d driver=lime -f 7100000 -s 2000000 -F CS16 -` | `cs16` |

`hackrf_transfer` runs from 2 Msps up; `-l` and `-g` are its two gains. The
Airspy R2 runs at 2.5 or 10 Msps, the Mini at 3 or 6. `airspy_rx -f` and
`airspyhf_rx -f` take megahertz, the others hertz. Each program's own
documentation says how to install it and the rest of its options.

A receiver installed as a service has no terminal to read from. There the
program writes into a FIFO, a named pipe, and a small service of its own
keeps it running. The FIFO sits in a directory of that service's, which the
receiver may read and not write: the feed runs as root, and nothing the
receiver does should be able to choose where root writes.

```ini
# /etc/systemd/system/fernsdr-feed.service
[Unit]
Description=Samples for FernSDR
Before=fernsdr.service

[Service]
Group=fernsdr
RuntimeDirectory=fernsdr-feed
RuntimeDirectoryMode=0750
RuntimeDirectoryPreserve=yes
ExecStartPre=/bin/sh -c 'test -p /run/fernsdr-feed/iq-hf || mkfifo -m 0640 /run/fernsdr-feed/iq-hf'
ExecStart=/bin/sh -c 'exec hackrf_transfer -r - -f 7100000 -s 2000000 > /run/fernsdr-feed/iq-hf'
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```ini
[band:hf]
source      = file
path        = /run/fernsdr-feed/iq-hf
format      = cs8
sample_rate = 2000000
center      = 7.1M
```

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now fernsdr-feed
```

The receiver waits for a writer on the FIFO and takes up again when the
program restarts; the FIFO is kept across its restarts, so it stays the one
the receiver reads. `path` names a file, so it is set in the file on the
machine, not in the admin panel. If a radio should rather work as the
RTL-SDR does, found by the setup and started by the receiver, [ask for a
module](https://github.com/Steven9101/FernSDR/issues/new/choose).

### Several bands from one radio

Standard input feeds one band. For several, use an external channelizer that
writes each IQ stream to its own FIFO or UDP port. Several independent radios
can feed separate FIFOs too. The producer's sample format, centre frequency
and rate must match each band's configuration:

```sh
mkfifo /tmp/iq-40m /tmp/iq-20m
```

```ini
[band:40m]
source      = file
path        = /tmp/iq-40m
format      = cs16
sample_rate = 1536k
center      = 7.1M
```

### Day and night bands on one radio

A radio that covers a few megahertz at a time can show 20 m in the day and
40 m at night. Configure both bands on the same device and give each its
hours; the receiver runs whichever band's hours it is and moves its
listeners to the other when they change:

```ini
[band:20m]
source      = module
module      = rtlsdr
sample_rate = 2400k
center      = 14.1M
hours       = sunrise-sunset

[band:40m]
source      = module
module      = rtlsdr
sample_rate = 2400k
center      = 7.1M
hours       = sunset-sunrise
```

In the admin panel this is one choice: on a band's page, under On the air,
pick In daylight, At night or Set times; the band sharing its input gets the
other hours in the same save. The hours are written to the configuration file
and apply at once, without a restart.

`hours` takes clock times in UTC (`06:00-18:00`, which runs past midnight when
the end is earlier than the start), `sunrise` and `sunset` at the station with
an offset if wanted (`sunset-1h-sunrise+30m`), and several ranges separated by
commas. Sunrise and sunset come from the grid square on the Station page;
without one they are taken as 06:00 and 18:00 UTC. Beyond the polar circles a
day without a sunset is all daylight and one without a sunrise all night.
Without `hours`, or with `hours = always`, a band is always on the air.

Two bands on one device whose hours overlap wherever the station is are
refused when the file is saved: clock hours that meet, a band that is always
on the air, or two bands with the same hours. Hours that meet only by the
Sun, somewhere in the year, are allowed; the band on the air first keeps the
device until its hours end, the other says it is off the air without a time,
and the log says so when the file is read. A band of its own with `hours`
simply stops outside them, which saves its CPU.

Off the air, a band is stopped on purpose: `/api/health` does not count it,
`fernsdr_band_on_air` in `/metrics` is 0, its page in the admin panel says
when it comes back, and the listeners' band list shows it greyed with the
time it comes on. Ten minutes before a band's hours end, its listeners are
told, and when they end they move to the band that takes the input over. A
decoder listening to the band starts again with a fresh clock when it comes
back. The schedule follows the system clock, so a machine without a real-time
clock picks the right band once NTP has set it.

### Decoding FT8

On the admin panel's Decoders page, one button installs the Fern-FT8
module and starts a decoder on the FT8 frequencies the bands cover. Each
decoder has two switches: *Shown to listeners* puts a Decodes tab on the
page, with a list and a map, and *Report spots to PSK Reporter* sends what it
decoded there, confident decodes only, each station at most once an hour
on each band.
In the file a decoder is a `[decoder:ft8]` section with its module and
channels; [Configuring a decoder](MODULES.md#configuring-a-decoder) has every
key. A decoder costs little: it gets a channel of at most 12 kHz cut from the
band's shared transform, not a band of its own.

### ka9q-radio

```ini
[band:80m]
source      = udp
bind        = 10.0.0.5
multicast   = 239.1.2.3
port        = 5004
rtp         = true
format      = cs16
sample_rate = 1536k
center      = 3.7M
```

Lost or reordered datagrams are counted rather than silently absorbed:
absorbing them makes an intermittent network look like an SDR fault.

A UDP input plays whatever reaches its port to every listener. When the port
is reachable from other machines, name who may send to it:

```ini
senders = 10.0.0.5, 10.0.0.6
```

It takes addresses and CIDRs, and `loopback`. Without it the band takes
everyone's samples and says so in the log when it starts. Datagrams from
anyone else are dropped, and the first of them is logged. This checks the
sender's address, which UDP does not prove: someone on the path can forge it.
`loopback` is safe, since the kernel drops loopback addresses arriving from
outside; for anything else, also `bind` to the interface the samples arrive
on and let a firewall keep the port closed to the rest.

### Replaying a recording

A recorded file replays at its true rate, which makes an intermittent fault
reproducible off-air:

```ini
[band:test]
source      = file
path        = /var/lib/sdr/capture.cs16
format      = cs16
sample_rate = 1536k
center      = 7.1M
loop        = true
```

## HTTPS

The server speaks plain HTTP by design. TLS belongs in front of it, where
certificates are already being managed and renewed.

This is not only tidiness. Browsers increasingly restrict what a page served
over plain HTTP may do, and one of the restrictions is the Web Audio API - a
receiver that works today over `http://` may be silent after a browser update.
Put TLS in front before that happens.

### Caddy

Two lines, and certificates are automatic:

```
sdr.example.org {
    reverse_proxy localhost:8073
}
```

### nginx

```nginx
server {
    listen 443 ssl http2;
    server_name sdr.example.org;

    ssl_certificate     /etc/letsencrypt/live/sdr.example.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/sdr.example.org/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:8073;
        proxy_http_version 1.1;

        # Required for the WebSocket upgrade.
        proxy_set_header Upgrade    $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host       $host;
        proxy_set_header X-Real-IP  $remote_addr;
        proxy_set_header X-Forwarded-For $remote_addr;

        # How the receiver knows the connection is encrypted. Without it the
        # session cookie lacks Secure and remote administration is refused.
        proxy_set_header X-Forwarded-Proto $scheme;

        # Buffering adds latency to a live audio stream for no benefit.
        proxy_buffering off;

        # The audio stream is continuous; do not time it out.
        proxy_read_timeout  3600s;
        proxy_send_timeout  3600s;
    }
}
```

`proxy_buffering off` matters. With buffering on, nginx accumulates the audio
stream before forwarding it and adds latency that looks like a server fault.
Even without it, the receiver sees only its own connection to the proxy: a
listener on a slow link shows in its bandwidth budget only once the proxy's
buffers for that listener are full, and a queue in the listener's network not
at all, since the round trip it reads is the one to the proxy.

This block was run rather than written from memory. Behind it, on a
self-signed certificate: the page loads, the WebSocket upgrades, the browser
reports a secure context so AudioWorklet is available rather than the
fallback, audio plays and the meter reads. `X-Forwarded-Proto` is accepted only
from a socket peer listed in `trusted_proxies`; it enables the `Secure`
admin cookie and remote administration. Keep the upstream port private when
using a proxy.

A proxy on this machine has to name the client, as both examples do. Without
`X-Forwarded-For` every visitor arrives as the machine itself: they share one
per-address listener limit and one sign-in lockout with the administrator, and
the check that keeps plain-HTTP administration to this machine takes them for
a browser on it.

### What the pages allow

Every page is served with a Content-Security-Policy: scripts, styles and fonts
come from the receiver itself, pictures and embedded pages from wherever the
operator's theme points. The admin page may not be shown in a frame at all.
The listener's page may be framed by the receiver's own pages; to embed it in
a club's website, name that site:

```ini
[server]
frame_ancestors = https://club.example.org
```

### Administration without HTTPS

Plain HTTP administration is allowed only for a direct connection from the
machine itself: the socket peer is loopback, no forwarding header is present,
and the browser asked for `localhost`, `127.0.0.1` or `[::1]`. From another
computer, an SSH tunnel does that:

```sh
ssh -L 9000:localhost:8073 receiver.example.org
# then open http://localhost:9000/admin
```

A receiver at home can let the other computers in the home network in over
plain HTTP too:

```ini
[admin]
home_network = yes
```

After a restart, a request also passes when no forwarding header is present
and it comes either from the machine itself, or from a private address
(10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16, fc00::/7 or
fe80::/10) that is not a proxy listed in `trusted_proxies`, and when the
browser asked for a name or address that only means something at home: an
address from those ranges, a name of one word such as `raspberrypi`, or one
ending in `.local`, `.home.arpa` or `.internal`. Nobody can register those
endings on the internet. A Fritz!Box calls the receiver `raspberrypi.fritz.box`
and OpenWrt `raspberrypi.lan`, but `fritz.box` is registered on the internet and
`.lan` could be, and a web page could point a name there at the receiver's
address; open `http://raspberrypi.local:8073/admin`, a name Raspberry Pi OS
answers to by itself, or the address instead. A name of one word is looked up
under the network's search domain, so where that domain is registered on the
internet to someone else, the address or a `.local` name is the safer choice.

Some things to know:

- The carrier-grade NAT range 100.64.0.0/10 does not count, so neither does a
  Tailscale address: use HTTPS or `ssh -L` there.
- A reverse proxy elsewhere in the home network that passes on requests from
  the internet has to be listed in `trusted_proxies` and send
  `X-Forwarded-For`. nginx sends no forwarding header unless told to, and names
  the receiver by its address, so an unlisted one would pass the internet off
  as the home network. Where that cannot be ruled out, leave the option off.
- Some routers rewrite the source address of connections they forward from
  the internet, which makes every visitor look like a neighbour. Browsers
  still ask for the receiver's public name and are refused, but anyone can
  send a home name by hand and reach the sign-in over plain HTTP. They all
  share the router's address, and with it one sign-in lockout.
- While failures from everywhere have closed sign-in, a computer in the home
  network gets in only if an administrator signed in from its network since
  the receiver started; the machine itself is not held back by failures from
  elsewhere.
- Leave the option off where you do not own the network, such as on a rented
  server, whose private neighbours are other customers.

Where neither HTTPS nor a tunnel is possible, the panel can be opened over
plain HTTP from anywhere, at your own risk:

```ini
[admin]
plain_http_anywhere = yes
```

The password itself still never crosses the network, but anyone between the
browser and the receiver (a public Wi-Fi, a provider) can read the panel,
and can change the page on its way to catch the password as it is typed. The
receiver says so in its log at every start, and the panel shows a red line on
every page, the sign-in included, while it is used this way. Like the rest of
`[admin]`, the setting can only be changed in the file on the machine.

### Tunnels

An HTTP-aware tunnel such as cloudflared behaves like the proxies above: it
names the client and the scheme, and runs on this machine, so the default
`trusted_proxies = loopback` fits it.

A raw TCP tunnel (`ssh -R`, frp in TCP mode, socat) is different. Every
connection through it comes from loopback, and whatever `X-Forwarded-For` or
`X-Forwarded-Proto` the far end sends arrives untouched, so anyone could name
any address or claim HTTPS. Set `trusted_proxies = none` for one. Listeners
then all share the tunnel's address, so the per-address limits have to be
off (`max_users_per_address = 0` under `[site]` and
`max_connections_per_address = 0` under `[server]`), with the far end limiting
its clients instead, and administration works over HTTPS in
front of the tunnel or through `ssh -L` as above. They share it for sign-in
lockouts too: a stranger's wrong guesses through the tunnel lock the
administrator out as well, until the lockout ends or the receiver restarts.

## Running it as a service

`install.sh` sets up the service for a release (see [Installing](#installing)).
For a build from source:

```sh
sudo tools/source-install.sh --service --prefix /opt/fernsdr
```

The script creates an unprivileged `fernsdr` user and installs
`fernsdr.service`. The configuration is `/opt/fernsdr/etc/fernsdr.conf`;
the binary and browser files are outside its writable directory. Existing
configuration is retained. Add `--no-start` to prepare the service without
starting or restarting it. The unit is still enabled for the next boot.

```sh
sudo systemctl status fernsdr
sudo journalctl -u fernsdr -f
sudo systemctl restart fernsdr
```

The unit hides hardware devices unless it was installed with `--usb`, which
allows USB devices and nothing else, for hardware modules. Otherwise run a
hardware driver separately and use a named pipe or UDP input. A named pipe used by separate services belongs
in a shared directory such as `/run/fernsdr`, not `/tmp`, because the receiver
has a private temporary directory. Give the receiver read permission and
supervise the producer as well. Keep the HTTP receiver on an unprivileged
port behind the HTTPS proxy.

The receiver will not serve as root: a program facing the internet around
the clock, which also starts module programs, should not own the machine if
something in it is ever found to be wrong. `--check`, `--version`,
`--hash-password` and `--set-password` still run as root. In a container with no other user, pass
`--allow-root`. A unit written by hand before 0.1.0 without `User=` now stops
at once and, with `Restart=always`, restarts in a loop: give it a user, as the
installer's unit does.

## Capacity

Two numbers decide how many people a receiver can carry, and for most sites
they are not the same one.

### CPU

Measure your sample rates and listener mix with `tools/loadtest.py` before
choosing a user limit. [PERFORMANCE.md](PERFORMANCE.md) explains the shared worker
pool and the cost of waterfall settings. Narrowband capacity does not predict
capacity for a 30 MHz input. Set `[server] dsp_workers` explicitly when a
container's CPU quota is smaller than its CPU affinity mask.

Wide bands run faster on transparent huge pages. The band's transform arrays
of a megabyte or more ask for them, which works when
`/sys/kernel/mm/transparent_hugepage/enabled` reads `[always]` or `[madvise]`,
the usual defaults; with `[never]` they run on ordinary pages. On the 64 Msps
test band huge pages saved about 6% of one core with 1,000 listeners.

The transforms use AVX-512 by default on AMD processors that have it (Zen 4
and later). On Intel they stay on AVX2: several Xeon generations lower the
clock of a core while it runs AVX-512 arithmetic, and with listeners on
every core that would slow everything else down. On an Intel CPU that keeps
its clock (Ice Lake and later are usually mild), `FERNSDR_FFT_ISA=avx512` in
the service environment turns it on; measure with `tools/loadtest.py` before
and after.

### Bandwidth

These are approximate per-user budgets; signal content and mode affect
compression, and transport headers add overhead:

| Profile | Audio | Waterfall | Total |
|---|---|---|---|
| Low | 32 kbit/s | 768 bins at 8/s | ~50 kbit/s |
| Balanced | 48 kbit/s | up to 1536 bins at 12/s | ~85 kbit/s |
| High | 64 kbit/s | 2048 bins at 20/s | ~180 kbit/s |

The user chooses the profile; the server enforces a ceiling regardless:

```ini
[band:40m]
max_user_bitrate = 100000
```

The server reduces waterfall traffic first. Persistent transport congestion
also reduces audio bitrate. The client displays the delivered rates.

**Sizing an uplink.** At the balanced profile, 200 users is about 17 Mbit/s.
On a 30-50 Mbit/s link that leaves real headroom. To carry more people on the
same link, lower `max_user_bitrate` - at 50 kbit/s, 200 users is 10 Mbit/s.

Cap concurrent users with:

```ini
[site]
max_users = 200
max_users_per_address = 16
```

Beyond that, connections are refused with a message the client displays, not
dropped silently. The second number keeps one machine from taking every
place: it counts listeners from one IPv4 address, or from one IPv6 /64, since
a host is usually given a whole /64. Behind a proxy it counts the addresses
the proxy reports, so a proxy missing from `trusted_proxies` makes every
listener look like one; the log says so when it refuses someone. 0 turns it
off, which a raw TCP tunnel needs (see Tunnels) and so does a load test from
one machine. A network that gives many people addresses from one /64, as some
campus and club Wi-Fi does, counts as one host here, so raise the number if
your listeners come from one; a chat mute and a sign-in lockout cover the
whole /64 in the same way. The chat's rate limit is counted the same way, so
reloading the page or opening more tabs does not reset it.

Below both, the server counts the connections each address holds, whatever
they are for, and refuses more than `max_connections_per_address` under
`[server]` (32 by default, 0 for no limit). Without it one address could open
every slot and keep it, and neither a listener nor you could get in. A
connection kept alive after an ordinary request is closed after 10 seconds
with nothing to do, and four slots beyond `max_connections` are kept for the
machine itself, so `ssh -L` still reaches the admin panel when the receiver is
full, unless a proxy on the machine brings the public in through them too. A
household given a whole IPv6 /48 may hold four times the limit across all of
it. This limit does not apply to a proxy listed in `trusted_proxies`, because
everyone arrives from it: set a per-client limit in the proxy, for example
`limit_conn` in nginx.

## The admin panel

Off unless you configure a password, because a panel with a default password is
a back door with a login page in front of it. `install.sh` and
`tools/source-install.sh` make one up for each receiver and print it. After
that, *Change password* on the Station page sets a new one. On the machine:

```sh
sudo /opt/fernsdr/current/fernsdr --set-password /var/lib/fernsdr/fernsdr.conf
sudo systemctl restart fernsdr
```

asks for it twice and writes its hash into `[admin]`, keeping the file's
owner; the receiver takes it when it restarts. In a container:
`docker exec -it fernsdr /opt/fernsdr/fernsdr --set-password /var/lib/fernsdr/fernsdr.conf`,
then `docker restart fernsdr`. Both read the password from the terminal rather than from an argument,
which would be in your shell history and visible in `ps` to every other user
on the machine. `--hash-password` only prints the two lines to paste into a
config by hand:

```ini
[admin]
password_hash = pbkdf2$600000$...
```

The hash signs an administrator in by itself, so keep the config readable by
the receiver's user alone: `chmod 600 fernsdr.conf`. The receiver warns at
startup when other users can read it, and writes it that way itself when the
panel saves it. It will not start at all with the configuration, the settings
saved beside it, the module directory or a band's waterfall archive inside the
document root or the uploads directory, since both are served to anyone, and
`/uploads/` serves only the images the panel stored there.

Then restart, and the panel is at `/admin` on the same port as the receiver.
For a remote browser, use the HTTPS proxy configuration above. Local HTTP
access works at `http://127.0.0.1:8073/admin`, and from the home network with
`home_network = yes` (see Administration without HTTPS).

What it does:

- **Setup.** The first sign-in on a receiver that has only the test band
  opens a setup of five steps: the password, the station and its place, the
  radio (found on USB by its ids, its module installed from the catalog),
  bands suggested from what the module says its radio tunes and the region's
  band plan, two of them taking turns by day and night, and whether the
  receiver is public and listed. It restarts the receiver to start the
  bands, where a service manager starts it again; otherwise it says to.
- **Live**: how many people are listening and to what, each listener's address,
  band, frequency, mode, stream rate and how long they have been connected, and
  every band's state. Refreshes every three seconds.
- **Restart a band.** For the usual failure - a source that has gone away, a
  pipe whose other end died - this is the fix, and it does not disturb anyone
  listening to the other bands. It returns immediately and restarts in the
  background, because stopping a band means joining a thread that may be
  blocked reading from the very source that has gone quiet.
- **Edit the configuration.** The text is parsed and used to configure a
  throwaway receiver before anything is written; if it would not start, the
  save is refused and you get the actual reason. The file is then replaced
  atomically. Station details, limits and the chat switch apply at once, the
  reply lists the band settings that wait for a band restart, and the rest
  waits for the receiver to restart. The password hash is shown as
  `(kept on the machine)` and stays in the file as it was.
  Some things can only be changed in the file on the machine, and a save that
  changes them is refused: who may administer (`[admin]`), which programs may
  run (`[modules]`), what is served and whose forwarded addresses are believed
  (`[server]`), every setting that names a file (`theme_file`, a band's
  `path` and `history_path`, and a module setting whose value is a path,
  such as the RX-888's `module.firmware`), where the receiver sends spots
  (`spot_server`), and where a UDP band listens and whom it takes samples
  from (its `bind`, `port`, `multicast` and `senders`). A
  stolen session is then limited to what the panel
  does while it lasts. A file setting may be deleted, which brings back its
  default, and a UDP band may be switched to another source or removed.
- **A band's page**: its live spectrum and last minute of waterfall, why it
  is not receiving, and its settings: name, starting audio quality, the
  ceiling per listener, the widest filter, the noise blanker, IQ swap and
  balance, removing the centre spike, the hours on the air, frequency
  correction measured against a known carrier, the S-meter calibration and
  the waterfall archive. For a hardware module: where it tuned, the gain it
  chose, clipping, dropped samples, its live settings and its log. *Add a
  band* offers the same suggestions as the setup.
- **Modules** and **Decoders**: install, update, switch off and remove
  modules from the catalog, find the radios a module sees, and with one
  button install the FT8 decoder for the FT8 frequencies the bands cover,
  show its decodes to listeners and report them to PSK Reporter.
- **Appearance** and **Widgets**: the page's colours and background picture,
  the default waterfall colours and meter, and the widgets beside the
  waterfall (chat, clock, space weather, greyline and lightning maps, the
  station card, listeners by band, notices, links, pictures, embedded
  pages).
- **Log**: what the receiver has been doing, problems first if asked.
- **Station details**: name, operator, location, grid square, band plan,
  antenna, contact, website, notice, the listener limit, how long an idle
  listener stays, the sdr-list.xyz listing, and *Change password*. The Station page keeps
  them in `fernsdr-settings.json` beside the configuration, and whichever was
  changed last wins. A value saved in the panel stands until that setting is
  edited in the file, by hand or in the configuration editor; saving the file
  with other changes leaves it alone.
- **Disconnect a listener.**
- **Updates**: the version running, a newer one when there is one, and
  moving to it (see [Updates](#updates)). Only a receiver installed from a
  release by `install.sh` updates itself; one built from source says so and
  offers nothing.
- **Backup**: one file with the receiver's bands, station details, look and
  pictures, and a receiver played back from one (see
  [Moving to another machine](#moving-to-another-machine)).

The chat can be turned off altogether with `chat = no` under `[site]`:
messages are then refused with that reason, nothing said is kept, and a chat
widget says the chat is off. Saving the configuration applies it at once.

How it is protected:

- The password is stored as salted PBKDF2-HMAC-SHA256 with 600,000 iterations;
  a hash made before that keeps its own count. Protect the configuration: a
  leaked hash permits authentication without recovering the password. It
  never leaves the machine, not even to the panel's own configuration page.
- Five wrong guesses within five minutes lock that address, or IPv6 /64, out
  for a minute. After that every wrong guess locks it out again for twice as
  long, up to 64 minutes, until a day passes without one or an administrator
  signs in from there. This, not the hash, is what actually stops a weak
  password being found.
- While 100 guesses from anywhere have failed in the last hour, only networks
  an administrator has signed in from since the receiver started may try, and
  a direct connection from the machine itself, such as through `ssh -L`. The
  machine is one network for all of this, whichever loopback address a
  program on it uses.
- A session is 256 bits of `/dev/urandom` in an `HttpOnly`, `SameSite=Strict`
  cookie, expiring after `session_hours` (8 by default, at most 168). JavaScript
  cannot read the cookie; `SameSite=Strict` restricts cross-site sending.
  Behind TLS it is also `Secure`.
- Login uses a single-use challenge. The browser derives the key with Web
  Crypto where available, keeping the expensive password derivation off the
  server's network thread. A challenge carries its own expiry and recipient,
  signed with a key the receiver draws when it starts, so asking for them
  stores nothing and cannot crowd out an administrator's. Answered challenges
  are kept until they expire; sessions and failed-login records have size
  limits.
- Mutations, including logout, require a signature covering the login
  context, monotonic counter, method, full request target and body. A signed
  request captured in one session cannot be used in a different session.
- The browser signs with a key made for its session from the password's
  derived key, and keeps only that. Whatever reads it out of the page, a
  script or an extension, holds a key that stops working when the session
  ends, not one that signs in again.
- An image upload is refused as soon as its first bytes show it comes from
  someone not signed in, rather than once all of it has arrived.
- The panel is a separate page from the receiver, so listeners never download
  it.

Remote administration requires TLS, except from the home network with
`home_network = yes`. The hashing tool refuses passwords under
twelve characters. An injected script could still act through a logged-in
browser, so request signing does not replace HTTPS or safe rendering.

### Admin API authentication

API clients POST `/api/admin/challenge`, derive PBKDF2-HMAC-SHA256
using the returned salt's **hex text** as salt bytes, and POST
`{ "nonce": "...", "proof": "..." }` to `/api/admin/login`. The proof is the
hex HMAC-SHA256 of the nonce text under the derived 32-byte key. The nonce is
opaque text: send it back exactly as it came.

The login reply includes `signing_context`. Retain the session cookie. The
session's signing key is the HMAC-SHA256 of
`fernsdr-admin-session-v3\n<signing_context>` under the derived key, so the
derived key itself never signs a request. Sign every mutation, including
`/api/admin/logout`, with HMAC-SHA256 under the session key over:

```text
fernsdr-admin-v3\n<signing_context>\n<counter>\n<METHOD>\n<path-and-query>\n<body bytes>
```

Here `\n` means one newline byte. Send the hex signature in
`X-FernSDR-Signature` and a strictly increasing positive integer, no greater
than 9,007,199,254,740,991, in `X-FernSDR-Counter`. Send mutations in sequence
and preserve the counter across page reloads.

A new password never crosses the network either. POST
`/api/admin/password` with `{"nonce","proof","hash"}`: a fresh challenge
answered with the current password, as for a login, and the new password's
stored form, `pbkdf2$<rounds>$<32 hex digits of salt>$<64 hex digits>`
with at least 100,000 rounds, made in the browser. The receiver writes it
into `[admin]` and ends every session, this one included.

A few more endpoints the panel uses, all behind the same session:
`GET /api/admin/hardware` lists the radios on USB and the kernel drivers in
their way; `POST /api/admin/restart` stops the receiver for its service
manager to start again, and is refused (409, with the reason) where nothing
would; `GET /api/admin/backup` returns the backup file and
`POST /api/admin/restore` plays one back, as
[Moving to another machine](#moving-to-another-machine) describes.

## Per-band settings worth knowing about

These live in a `[band:...]` section, and all of them can also be set from the
admin panel, where the panel's value wins and outlives the file.

### The S-meter calibration

```ini
calibration = 14.1M:-23.5, 28M:-29
```

What to add to a dBFS reading to get dBm, at the frequencies you measured it.
Feed a known level into the antenna socket and subtract: a generator putting
out -73 dBm that reads -50 dBFS is an offset of -23. More than one point is
interpolated between, which is what a band needs when the antenna or the front
end is not flat; beyond the outermost point the value is held rather than
extrapolated.

With no calibration the meter is a relative indication and says so. That is
not the same as an offset of zero, and the difference matters: S-units quoted
from an uncalibrated meter are not a signal report.

The admin panel has a guided version. State what you are feeding in and where,
press the button, and the receiver reads itself.

### The waterfall archive

```ini
history = private
history_hours = 24
history_bins = 1024
history_interval = 1
```

A rolling record of the band, in a file of fixed size. `off` is the default,
`private` means a signed-in admin only, and `public` means anyone with the
link.

Two things to decide before switching it on. The file is created at full size
immediately, so you know what it costs: 24 hours at 1024 bins and one line a
second is 89 MB per band, and it cannot grow past that. And a record of a band
is also a record of who transmitted when, so making it public is a decision
rather than a setting.

Changing the retention or the width starts the record again. Changing who may
read it does not. Neither does a frequency correction smaller than a quarter
of one of the archive's cells; re-tuning the band, or a larger correction,
starts it again, because the old lines would be drawn beside where their
signals were. The frequencies the record was made for are kept beside it, in
the archive's path with `.span` added; move both together.

The History tab offers spans from 15 minutes to 24 hours. It uses recorded
timestamps and capture frequencies, including gaps while a band was stopped.
Long ranges are sampled to keep each response bounded; short transmissions
can be missed in a day overview. Select a shorter span for finer time detail.
`history_path` chooses the archive location even if history is initially off.

## Listing the receiver

sdr-list.xyz is a public directory of web receivers, on a map and in a list.
To be in it, switch on "List on sdr-list.xyz" on the admin panel's Station
page, or set it in the configuration:

```ini
[site]
grid        = JO62qm
sdr_list    = yes
public_host = sdr.example.org
# Only when listeners use another port than the receiver listens on,
# for example behind a proxy:
# public_port = 443
```

The listing needs the grid square and the address listeners use, a host name
or IPv4 address without `http://` or a port. Once a minute the receiver tells
sdr-list.xyz its name, grid square, antenna, address and port, the frequencies
its bands cover, and how many listeners it has and allows. It does not say
which version of FernSDR it runs. The directory moves the pin on its map a
little, so the exact location stays private.

The report goes out with `curl` over HTTPS, so curl has to be installed and
the machine has to reach the internet, through the proxy in `https_proxy` if
there is one. The Station page says whether the last report arrived, and why
not if it did not. While sdr-list.xyz cannot be reached, the receiver tries
again after one, two, four and eight minutes, then every fifteen.

To leave the directory, switch the listing off: the receiver stops reporting,
and sdr-list.xyz drops it five minutes after the last report. The directory
checks from time to time that the receiver's page answers at the address and
port it was given.

## Monitoring

```sh
curl -s localhost:8073/api/status | jq
curl -sf localhost:8073/api/health || echo "a band has stopped"
```

`/api/health` returns 503 when any band is not receiving: its source has
ended, or its module is failing and being retried, or is waiting for you to
fix a setting. A module you switched off in the admin panel does not count,
nor does a band that its hours have taken off the air.
Use it to alert someone rather than to restart the service automatically:
the receiver already retries a module band on its own, and restarting the
whole receiver does not plug a dongle back in.

If you already run Prometheus, `/metrics` is there in its text format:

```sh
curl -s localhost:8073/metrics
```

It carries listeners against the cap, audio and waterfall bitrate, frames
dropped for sockets that fell behind, and per band whether it is running, how
many are on it and its noise floor. It says nothing about who any individual
listener is: no addresses, no frequencies. It is unauthenticated, like
`/api/status`; refuse it at the proxy if you would rather it were not public.

For a deeper check, `tools/fernsdr-probe.py` connects as a real client, decodes
both streams and reports what actually arrived:

```sh
tools/fernsdr-probe.py --host localhost --freq 7100000 --mode usb --seconds 10
```

It needs only the Python standard library, so it runs on the receiver itself.

## Tuning

Most per-band settings are derived from the sample rate and are usually right.
The ones worth knowing:

| Setting | Default | When to change it |
|---|---|---|
| `usable_fraction` | 0.8, 0.94 for `signal = real` | How much of the sample rate to show. Raise if your front end's anti-alias filter is sharper than usual. |
| `spectrum_bins` | 2× the channelizer FFT, at most 65,536 | The band's own waterfall line. A listener zoomed in past its bins gets a finer spectrum of their own view, so raising this is rarely worth what it costs every block. |
| `spectrum_averages` | 8 | More averaging steadies the display and lowers the waterfall bitrate. Averages that would overlap by more than 75 % are left out: on a 2 Msps band that is 5, and the line is as steady. |
| `spectrum_smoothing` | 0.5 | Same, applied across lines. The variance it removes is noise the codec would otherwise spend bits encoding. |
| `max_bandwidth` | 20 kHz | The widest passband one listener may request. |
| `max_user_bitrate` | 100000 | The ceiling described above. |
| `wfm` | where the band shows 64 to 108 MHz | Offers wide FM, with RDS, on a band of 240 kHz or more that shows FM broadcasting; `yes` offers it anywhere, `no` nowhere. |

## Troubleshooting

**No audio, waterfall fine.** The browser needs a gesture before it will play
sound; the client shows a "Start audio" control. If it is already running,
check `/api/status` for the band, and check that the reverse proxy is not
buffering.

**Audio breaks up.** Look at the buffer figure in the status bar. Rising
dropout counts with a healthy buffer point at the client's machine; a buffer
that will not fill points at the network. The low bandwidth profile raises the
buffer target as well as lowering the rate.

**Waterfall is blank but audio works.** Almost always a proxy that is not
passing WebSocket binary frames intact, or one buffering them. A browser
without WebGL2 draws the waterfall with a slower fallback, which is a display
difference, not a fault.

**The band stops after a while.** Check the source process and the receiver
log. A source EOF stops that band and makes `/api/health` return 503; the
server stays available for diagnosis and an admin band restart. Systemd only
restarts a process that exits, so monitor band health as well as service state.

**A module band says offline.** The band's row on the Overview page says why,
in the module's own words, and when it tries again. *No RTL-SDR is plugged
in*: check the cable and `lsusb`. *In use by another program*: stop the
other program, often a leftover `rtl_sdr` or `rtl_tcp`. *Waiting for the
operator* means retrying cannot help: a setting is wrong, or the module is
switched off or not installed; fix it and restart the band. *Permission
denied* on a service built from source: `sudo tools/source-install.sh
--service --usb`.

**Everything is slow with many users.** Check the uplink before the CPU. At
200 users on the balanced profile you are asking for about 17 Mbit/s sustained.
