# Changelog

## 0.1.0 (not yet released)

The first release.

### Receiver

- Demodulates USB, LSB, CW, CW-L, AM, synchronous AM, NFM and DSB, each with its
  own passband presets and a passband that can be dragged to any width; the
  SSB presets include 2.7 kHz, flat from 300 to 2700 Hz. Wide FM, in mono,
  on a band of 240 kHz or more, demodulated once per station for everyone
  tuned to it, with RDS: the station's name, programme type and radiotext.
- Runs several bands at once from standard input, files, UDP, ka9q-radio
  multicast, a synthetic test source or a hardware module; the RTL-SDR,
  RX-888 and SDRplay modules are installed from the admin panel, and a
  direct-sampling band of 129.6 Msps from the RX-888 takes about three
  quarters of a core of a Ryzen 9 9950X held to AVX2.
- Hours on the air for each band, as UTC times or by sunrise and sunset at
  the station's grid square, so that one radio serves 40 m at night and 20 m
  by day; listeners are told ten minutes before and move with the input, and
  the hours apply without a restart.
- Decoder modules on a narrow channel of a band's shared spectrum, sandboxed
  and at idle priority; Fern-FT8 is the first. Decodes are checked before
  they are kept for a day, shown to listeners where the operator makes the
  decoder public (a list, a filter and a map, one click from being tuned),
  served at `/api/decodes`, and reported to PSK Reporter at a switch.
- Shares one FFT front end per band across its listeners, using AVX-512 on AMD,
  AVX2, SSE2, NEON or plain C++, whichever the machine has. A band nobody
  listens to and that keeps no archive skips the listeners' transform and
  makes only four waterfall lines a second, back to every line as soon as a
  listener arrives, and the waterfall averages no transforms that overlap by
  more than 75 %: an idle 2 Msps band takes a fifth of the CPU it did.
- Per listener: automatic gain in four speeds, each set against the band noise
  around the channel so that the noise stays level between words and a weaker
  station taking over is up within half a second, following the carrier on
  AM, or manual gain; noise reduction, an automatic notch (manual notches
  through the protocol), an automatic squelch that needs no threshold, an
  audio low cut and NFM de-emphasis.
- Per band: noise blanker, DC removal, IQ balance and swap, frequency and ppm
  correction measured against a known carrier, S-meter calibration in dBm, and
  an optional waterfall archive (the panel offers 6 to 72 hours).
- Audio in NAC3, coded against the channel's own noise, and a waterfall in
  WFC5, rows through a context-modelled range coder that takes about a third
  less than WFC4 (still sent to older pages), with three data profiles for
  listeners on slow links.
- An optional listener timeout: a listener who has not touched anything for
  the set minutes is asked whether they are still listening, and their place
  is freed a minute later if nobody answers.
- Raises its own limit on open files at start, so more than about a thousand
  listeners can connect without the service having to be configured for it.
- A bandwidth budget per listener that gives up waterfall before audio when
  the link is too slow, and tells a stall from a slow link: after Wi-Fi
  retries, a phone changing cells or a connection that drops out for a
  moment, the waterfall and the audio are back in full within a few seconds.
  A queue building up in the network, which shows in the connection's round
  trip, takes the waterfall and not the audio.

### Listener page

- Drawn like the admin panel: Inter (48 KB, fetched once, the system's font
  until it arrives), its greys in both themes, controls grouped on filled
  surfaces under a quiet heading, the panels' tabs and every choice as one
  well with the chosen option raised, and switches and sliders as there.
  The waterfall, the dial and the axes keep their monospaced figures, and an
  operator's colours from the Appearance page still apply.
- Waterfall and spectrum with peak hold and four colour maps (Classic unless
  the operator or the listener picks another), tuning by drag, tap, wheel,
  digit or typed frequency, and a phone layout with a sheet of controls.
- The band plan of the station's IARU region, or of the United States,
  Canada, the United Kingdom, Germany, Australia or Japan, chosen on the
  Station page or worked out from the grid square: amateur segments with
  their band ("40 m, CW"), broadcast, utility and licence-free allocations
  under them, and markers for the FT8, FT4, WSPR and JS8 dials, beacons and
  time stations that tune to them when tapped. Every row cites its source,
  a test checks the files for gaps and overlaps, and a receiver loads only
  the few kilobytes its plan needs, after the page is up.
- Tools under the dial for everyone: VFO A and B, bookmarks (kept in the
  browser, exported and imported as a file), recording what you hear,
  a listening log exported as ADIF (marked as heard, not worked) or CSV, and
  a link to the listener's own radio over its CAT cable (Kenwood, Elecraft
  and FlexRadio, Yaesu, the FT-817 family, Icom CI-V) that follows its dial,
  sets it, or both. Each loads the first time it is opened.
- Works with CATSync and the programs like it that keep a transceiver and
  a web receiver on the same frequency and mode: to them the page looks like
  a PA3FWM WebSDR, so the radio's dial tunes the receiver and a click on the
  waterfall tunes the radio.
- Full controls, a switch in Display, add bass and treble (kept per kind of
  mode, applied in the browser), the audio low cut and NFM de-emphasis.
- The band list names what each band listens with (RX-888, RTL-SDR,
  SDRplay, a recording, a network stream) and gives its range in MHz; from
  ten bands on it has a search by name or by a frequency inside a band.
- Keyboard shortcuts for tuning, volume, mute, zoom, filter width, mode,
  typing a frequency and noise reduction; `?` lists them.
- Remembers frequency, filter and zoom per band; links open on the same signal
  and view. Tuning that takes the passband off screen, by key, wheel or a
  typed frequency, brings the view along with the passband in its middle.
- A chat in which frequencies are links, and widgets the operator chooses:
  among them space weather (NOAA's scales, solar flux, sunspots, Kp and A,
  X-rays, the solar wind, and for each band from 160 m to 6 m whether it is
  open here, from the nearest ionosonde's MUF), fetched by the receiver
  every ten minutes so that no listener's browser talks to anyone else, and
  a lightning map centred on the station's locator, a day and night map
  with the greyline, the station and its sunrise and sunset, and a clock with
  UTC and the listener's own time, and a station card with the region
  around the receiver, its locator, antenna and what it listens with.
  Widgets showing another site's page load it only when the listener asks,
  so no third-party page, cookie banner or tracker loads with the receiver.
- A layout of the listener's own, kept in their browser: the meter they
  read, the controls on the left or the right, and whether the meter, the
  volume, the band name and the tools sit on the dial. The operator's theme
  sets the look and the meter a listener starts with; one button goes back
  to it.
- An S-meter as a bar, a needle or numbers, with a trace of the last minute.
  In SAM the Receive panel and the meter say whether the carrier is locked
  and how far it sits from the dial.
  In NFM the Receive panel names the station's CTCSS tone, measured by the
  receiver over two seconds, and a narrow notch at its measured frequency
  takes it out of the audio unless the listener switches that off. With
  full controls, a tone squelch keeps NFM silent until a chosen tone is
  received, so only one repeater's or group's users are heard.
- The waterfall archive in its panel or full size, filling the window, with
  its times on the clock (14:00, 14:10 rather than evenly spread) in UTC or
  local time and the date at midnight. Full size, six hours and more are
  drawn as tall as the day needs, two pixels a minute, and scrolled like a
  full-day page; its pieces load as they come into view, about 3 MB for a
  whole day of a 2.5 MHz band.
- The note that the receiver sends fewer waterfall lines to stay in its
  bandwidth appears after five seconds of it and stays until fifteen seconds
  at the full rate, rather than blinking with every reading.
- Playback that holds its pitch steady while it keeps the delay short: about
  230 ms from antenna to speaker in the benchmark lab, on a plain-HTTP page.
  After a slow or broken stretch of connection the extra buffer it took is
  given back within seconds once audio arrives evenly again.

### Operator

- Admin panel at `/admin`: live bands and listeners, band and station
  settings, appearance and widgets, hardware modules and decoders, the log
  and the configuration file. Sign-in is a challenge-response over a PBKDF2
  hash, and every change is signed.
- A setup at the first sign-in: the password, the station and its place,
  the radio found on USB with its module installed from the catalog, bands
  suggested from what the radio tunes and the region's band plan (two of
  them taking turns by day and night), and whether the receiver is public
  and listed. The panel restarts the receiver where a service manager starts
  it again.
- A new admin password from the Station page, without the password crossing
  the network, from `fernsdr --set-password` on the machine, or by running
  the installer again.
- The chat can be turned off with `chat = no` under `[site]`.
- Lists the receiver on sdr-list.xyz when the Station page or `sdr_list = yes`
  asks for it, with a report once a minute and its outcome on the page.
- `/api/status`, `/api/health` and `/metrics` for monitoring.
- The band's page shows where a hardware module tuned, the gain it chose and
  how much of the time its converter clips; the Overview flags a band that
  clips, and the log warns about one clipping at a gain set by hand.
- Released as static programs for x86_64, aarch64 and armhf (ARMv7 with
  NEON), which run on any Linux of their processor.
- `install.sh`, published with each release, installs it as a service after
  one question, at home, on a server with a domain name (setting up HTTPS
  with Caddy under systemd, where the distribution packages it) or on one
  without, moves over a
  receiver set up from source, and when run again updates it or makes a new
  admin password. Where Docker runs it offers the container instead, and
  updates that too, going back to the image before if the new one fails. Tested on
  Debian 10 to 13, Ubuntu 18.04 to 26.04, Fedora, Rocky 9, AlmaLinux 8,
  Amazon Linux 2023, openSUSE Leap and Tumbleweed, and Arch; on systemd
  before 239 (Ubuntu 18.04) its units run without the system call filter,
  which that systemd cannot express. Where there is no systemd, as on
  Alpine (OpenRC, with BusyBox's tools and mdev), Void (runit), Devuan and
  MX Linux (SysV init), Gentoo or Slackware, it sets up `fernsdr
  --supervise` instead, which runs the receiver as its own user, starts it
  again when it stops, clears away modules it left running, and carries out
  the admin panel's updates with the same trial and rollback; on a machine
  with none of these inits it starts it and says what to add to the boot
  script. `tools/source-install.sh` builds, tests and optionally installs it from
  source.
- A container image, built from a release's own files on Alpine (34 MB): it
  writes a configuration and a password into its volume on the first start,
  runs the receiver as its own user under tini, and gives USB devices passed
  with `--device` to it; the Updates page says that a new image is the
  update there.
- Updates from the admin panel's Updates page, never by themselves. After
  signing in, the panel asks the receiver to look once a day and offers a
  newer version in a dialog with its notes, set as headings and lists. A new
  version is kept once it has served for a minute with every band that ran
  before; otherwise the version before comes back, with its configuration
  files as they were, and so it does when the machine goes down mid-update.
- A backup file from the Updates page moves a receiver to another machine:
  restored there in the first step of the setup, it brings the bands,
  station details, look and pictures, installs the modules the old machine
  used and restarts, while the new machine keeps its own address, module
  folder and password.

### Security

- Plain-HTTP administration only from the machine itself, or from the home
  network with `home_network = yes` under `[admin]`; everything else needs
  HTTPS.
- Releases are signed with Ed25519, written for this project from RFC 8032
  and stricter than it, and a receiver takes only those signed by a key it
  carries. The updater runs as root in a unit of its own, never in the
  receiver, which cannot write to its own program.
- The configuration page never shows the password hash, and cannot change
  `[admin]`, `[modules]`, `[server]`, a setting that names a file (a
  module's setting holding a path among them), where spots are sent, or
  where a UDP band listens; those are changed in the file on the machine.
  The log keeps each message on one line, whatever it quotes.
- Sign-in limits count by network and across networks. After the fifth wrong
  guess every further one doubles the lockout, up to 64 minutes, and the
  machine itself can still sign in when failures from everywhere have closed
  sign-in to strangers. Sign-in challenges are signed rather than stored, so
  asking for them cannot crowd out an administrator's.
- The receiver will not start with its own files inside a directory it
  serves, and `/uploads/` serves only uploaded images.
- A Content-Security-Policy on every page, and the admin page is never framed.
- The chat shows names, messages and addresses as text; only a frequency
  becomes a button. A test holds it to that against the attack behind
  CVE-2026-97723, a chat message that ran script in every listener's page in
  UberSDR before 0.1.58, which magicint1337 found and reported.
- At most 16 listeners per IPv4 address or IPv6 /64 by default.
- The configuration and settings are written readable by their owner only.
- The receiver refuses to serve as root unless given `--allow-root`.
- The HTTP, WebSocket, JSON, configuration, module package, Ed25519, ustar
  and release manifest parsers have libFuzzer targets
  (`make -C server fuzz`). The first runs found that JSON
  numbers with a fraction were written with ten significant digits and that
  an integer too large in the configuration was cast regardless; both are
  fixed.

### Upgrading from a build before 0.1.0

- Build the client together with the server. An older client sets its colour
  scheme with an inline script, which the new page policy blocks; the
  installer refuses to pair one with this server.
- A systemd unit written by hand without `User=` now stops at once and, with
  `Restart=always`, restarts in a loop. Give it a user, as the installer's
  unit does.
- Behind a proxy on another host, list it in `trusted_proxies`, or every
  listener looks like one address to the new per-address limit.
- The limit counts an IPv6 /64 as one address. A campus or club Wi-Fi that
  gives everyone an address from one /64 can reach it; raise
  `max_users_per_address` there. A chat mute or a sign-in lockout covers the
  /64 too.
- A configuration saved from the panel is readable by the receiver's user
  alone. Read or edit it as that user, or with `sudo`.
- `[admin]`, `[server]`, `theme_file`, a band's `path` and `history_path`,
  and a UDP band's `bind`, `port` and `multicast` are now changed in the file
  on the machine only; the panel refuses a save that changes them.
- A receiver whose configuration, saved settings, module directory or
  waterfall archive lies inside the document root or the uploads directory
  now refuses to start and says which; move it out.
