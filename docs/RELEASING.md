# Releasing

A release is what `install.sh` installs and what a receiver's Updates page
offers: for each of x86_64, aarch64 and armhf an archive with the program,
the pages, the systemd units, an example configuration and the licences; a
manifest naming each archive by size and SHA-256; the manifest's Ed25519
signature; and `install.sh` itself, with the keys the programs check
releases with. They are published as the assets of a GitHub release, where
`https://github.com/Steven9101/FernSDR/releases/latest/download/` finds the
newest one. Downloads from a private repository fail, so nothing installs or
updates until the repository is public. A container image built from the
same archives goes to `ghcr.io/steven9101/fernsdr`.

The formats are in `server/src/update/release.h` and `ustar.h`; how a
receiver updates, and how it goes back, is in [DEPLOYMENT.md](DEPLOYMENT.md#updates).

## The keys

A receiver takes a release signed by any key in
`server/src/update/release_keys.h`, and nothing else. There are two: one that
CI signs with, and one kept offline for the day the first is lost or leaks.
A release that changes the list reaches receivers signed by a key they
already have, so with the offline key a leaked key can be replaced, and a
receiver that has taken that release refuses what the old key signs.

The two keys are in place: CI's,
`59928fb2118b5d95360a86fad202c42a009b323ebcc41632dfd1ae29c02007dd`, and the
offline one, `c0c7529f3d64898a19864a5f87a82155589d6ff298ab757d0076b5ab50f5e9ab`.
The `release` environment holds CI's secret and takes deployments from `main`
only. A required reviewer on it needs a paid plan while the repository is
private; add one when it is public. A build without keys (a fork's, say)
takes no update at all and `make installer` makes no installer.

To make new ones, on a machine you trust:

```sh
make -C server release-tools
server/build/fernsdr-release keygen ci.key        # prints the public half
server/build/fernsdr-release keygen offline.key   # and this one's
```

Each secret file holds 64 hex digits and is readable by its owner only.

1. Put both public halves in `release_keys.h`, as
   `ReleaseKey{0x3d, 0x40, ...}`, and commit that.
2. In the repository's settings, make an environment called `release`:
   deployments from `main` only, with yourself as a required reviewer. Give it
   a secret, `FERNSDR_RELEASE_KEY`, holding the contents of `ci.key`.
3. Keep `offline.key` off networked machines, on paper or a USB stick in a
   drawer, and a copy of `ci.key` with it. Then delete both from the machine
   they were made on.

`fernsdr-release sign` refuses a secret whose public half the build does not
carry, so a wrong secret in the environment stops the release rather than
signing one no receiver takes.

## Making one

1. Set the version in `server/src/version.h`, and head its section of
   `CHANGELOG.md` `## 0.1.1 (2026-10-01)`: the release takes its date and its
   notes, the section's text, from there.
2. Rehearse. Start the Release workflow from the Actions tab without
   *publish*. It builds and tests everything as a release, the ARM builds
   under qemu-user, and signs with a key made for the run, which that run's
   programs and installer trust instead of the real ones. The files are the
   run's artifact. `tools/build-release.sh rehearsal OUT` does the same on
   your own machine, given the cross compilers and qemu-user.
3. On a machine with Docker, run the labs against the source as it will be
   released:

   ```sh
   sudo tools/install-lab.sh
   sudo tools/update-lab.sh
   ```

   The first installs, updates and moves over receivers on the Debian,
   Ubuntu, Fedora, Rocky Linux, AlmaLinux, Amazon Linux, openSUSE and Arch
   Linux releases it names, each booting systemd in a container, and on
   Alpine, Void and Devuan with OpenRC, runit and SysV init; the second puts
   the updater through updates that work, fail and are interrupted
   (`LAB_INIT=sysv` for the same without systemd). Both sign with keys of
   their own and change nothing on the machine but containers and a work
   directory under /tmp; the checkout stays as it was.
   `make -C server docker` and `tools/docker-lab.sh` do the same for the
   container image.
4. Start the Release workflow with *publish* and approve the `release`
   environment when it asks. One job builds the pages with npm, with the
   packages' install scripts switched off, and hands them on as files. The
   release job, which runs nothing but this repository's code and the
   system's compilers, builds and tests the programs, packs the archives,
   signs the manifest in a step of its own, the only one given the key, and
   publishes the release `vVERSION` at the commit it was built from. The
   programs are built there because every receiver's updater runs them as
   root: a package from npm with something in it could reach the pages, but
   not them, and not the key.

A receiver sees the new version once someone presses *Check for updates* on
its Updates page; nothing updates by itself.

5. The workflow then builds the container image from the archives it just
   published, for all three processors, with `tools/build-image.sh`, and
   pushes `ghcr.io/steven9101/fernsdr:VERSION` and `:latest`. The image
   carries the signed release's own files; nothing in it is built anew. The
   rehearsal builds the images too, without pushing them. A new package on
   ghcr.io is private: once, after the first release, make it public in the
   package's settings on GitHub, or `docker pull` asks everyone to sign in.
   By hand, from the release's archives on a machine with Docker and buildx:
   `tools/build-image.sh OUT ghcr.io/steven9101/fernsdr push`, where `OUT`
   holds what `tools/build-release.sh` made.
6. A release that comes with a new benchmark round gets the round's runs by
   hand: they are kept only on the lab machine, and the workflow neither
   packs nor uploads them. Pack each run's files without the audio captures
   and attach the archive with `gh release upload vVERSION FILE`, then name
   it in README.md and bench/README.md. For 0.1.0 it is
   `fernsdr-bench-runs-20260929.tar.xz`.

## What a mistake costs

A release cannot be taken back from receivers that installed it, but each
one keeps a new version only once it has run for a minute with every band
back, and goes back to the version before otherwise. A bad release that runs
is replaced by publishing a newer one. Deleting a release on GitHub makes the
one before it the newest again for anyone installing; a receiver never moves
to an older version, so one that took the deleted release keeps it until a
newer one appears.
