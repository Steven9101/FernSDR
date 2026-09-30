# Principles

How FernSDR is built. Every change is held to these; a change that breaks
one needs a reason written down next to it, and most such changes should
not be made.

## Simple for everyone, deep for those who want it

Most listeners want to tune, hear and see the waterfall. Some want every
filter edge and a CAT link to their rig. Both are served by one page:

- The listener page opens in **Essential**: the controls everyone uses.
  **Full** adds sound shaping and fine filter controls. A listener chooses
  once and the page remembers.
- A feature not everyone needs is out of sight until asked for (a tab that
  appears only when there is something in it, a tool behind a pill under the
  dial, a panel loaded on first use). It is never left out for those who do
  want it.
- The admin panel works the same way: defaults that are right for most
  receivers, the everyday settings on its pages, and everything else in the
  configuration editor.

## Setting up is one step

- Installing works on any Linux distribution with any init system, for
  someone who has never used a terminal and for someone who wants to know
  every file: `install.sh`, the release archive, the container, or from
  source, all documented in [DEPLOYMENT.md](DEPLOYMENT.md). Updates are a
  button in the admin panel.
- An optional part is a switch or a short form in the admin panel, not a
  list of commands: installing a module, giving a device to a band, turning
  on FT8 decoding, reporting spots. The panel offers what makes sense for
  this receiver (the FT8 frequencies its bands cover, the devices it finds)
  and applies the change without a restart where it can.
- Everything the panel does, apart from the look, can also be written in
  the configuration file, for those who keep their setup in version
  control. The panel writes the file, except for the station details and
  the look, which it keeps in `fernsdr-settings.json` and
  `fernsdr-theme.json` beside it; for a station detail, whichever of the
  two changed last wins. A copy of a receiver's setup needs all three
  files, as the backup on the Updates page has them (DEPLOYMENT.md,
  "Moving to another machine").
- When something cannot work (a band too narrow for broadcast FM, a vendor
  library not installed), the page and the panel say why and what to do,
  in words, instead of failing quietly.

## Modular

- Hardware, decoders and future extensions are **modules** behind a
  versioned contract ([MODULES.md](MODULES.md)): input modules bring samples
  in, decoder modules turn channels into decodes. The core does not grow
  for one device, one mode or one service.
- Modules run in their own processes with no more access than they need
  (decoders: seccomp socket and signal limits, Landlock file limits where
  available, lowest priority). What a module sends is
  checked field by field before anyone sees it, and no module code or markup
  ever reaches a listener's page.
- Features inside the page are loaded when first used, so the page a
  listener opens stays small however much FernSDR can do.

## No dependencies in the core

- The server is C++17 and the operating system: no libraries to install, no
  package manager, static builds for x86-64, aarch64 and armhf.
- Modules, tools, tests and the build of the web pages may use more. A
  module that must load a vendor's library does so at run time and says so
  in its manifest's `requires`.
- New code reuses what is there before it adds anything.

## Efficient, measured

- CPU, memory, latency and bandwidth are features. A change that affects
  them is measured before and after, on the same input, and kept only if it
  is at least as good; the numbers go in the commit message or
  [PERFORMANCE.md](PERFORMANCE.md).
- Work is shared wherever listeners share it: one transform per band, one
  demodulation per broadcast station, one decoder channel per frequency.
- Hot loops are written for vectors, chosen at run time (AVX2 or AVX-512 on
  x86, SSE2 or NEON otherwise) with a plain fallback, and tested on every
  instruction set and on ARM.

## Correct and safe by default

- Every fix comes with a test that fails without it. Threaded code is run
  under ThreadSanitizer, parsers and the rest under AddressSanitizer and
  UndefinedBehaviorSanitizer, and the suite runs on ARM under emulation.
- Anything that exposes the receiver more (a public admin panel over plain
  HTTP, publishing decodes, reporting spots) is off until the operator turns
  it on, and says what it means when they do.

## Honest

- Numbers come from raw data kept with the repository. Where another
  receiver does better, the release page says so.
