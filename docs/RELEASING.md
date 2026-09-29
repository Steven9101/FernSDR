# Releasing

A release is what `install.sh` installs and what a receiver's Updates page
offers: for each of x86_64, aarch64 and armhf an archive with the program,
the pages, the systemd units, an example configuration and the licences; a
manifest naming each archive by size and SHA-256; the manifest's Ed25519
signature; and `install.sh` itself, with the keys the programs check
releases with. They are published as the assets of a GitHub release, where
`https://github.com/Steven9101/FernSDR/releases/latest/download/` finds the
newest one. Downloads from a private repository fail, so nothing installs or
updates until the repository is public.

The formats are in `server/src/update/release.h` and `ustar.h`; how a
receiver updates, and how it goes back, is in [DEPLOYMENT.md](DEPLOYMENT.md#updates).

## The keys

A receiver takes a release signed by any key in
`server/src/update/release_keys.h`, and nothing else. There are two: one that
CI signs with, and one kept offline for the day the first is lost or leaks.
A release that changes the list reaches receivers signed by a key they
already have, so with the offline key a leaked key can be replaced, and a
receiver that has taken that release refuses what the old key signs.

While the list is empty, as it is until the first release, a build takes no
update at all and `make installer` makes no installer.

To make them, on a machine you trust, once:

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
   their own and change nothing on the machine but containers.
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

5. The container image is not built by the workflow yet. From the release's
   archives, one per processor, on a machine with Docker and buildx:

   ```sh
   for arch in x86_64 aarch64 armhf; do
       mkdir -p image-$arch/tree && tar -xf fernsdr-VERSION-linux-$arch.tar -C image-$arch/tree
       cp server/docker/Dockerfile server/docker/entrypoint.sh image-$arch/
   done
   docker buildx build --platform linux/amd64 -t ghcr.io/steven9101/fernsdr:VERSION-amd64 --push image-x86_64
   docker buildx build --platform linux/arm64 -t ghcr.io/steven9101/fernsdr:VERSION-arm64 --push image-aarch64
   docker buildx build --platform linux/arm/v7 -t ghcr.io/steven9101/fernsdr:VERSION-armv7 --push image-armhf
   docker buildx imagetools create -t ghcr.io/steven9101/fernsdr:VERSION -t ghcr.io/steven9101/fernsdr:latest \
       ghcr.io/steven9101/fernsdr:VERSION-amd64 ghcr.io/steven9101/fernsdr:VERSION-arm64 ghcr.io/steven9101/fernsdr:VERSION-armv7
   ```

   The image carries the signed release's own files; nothing in it is built
   anew.

## What a mistake costs

A release cannot be taken back from receivers that installed it, but each
one keeps a new version only once it has run for a minute with every band
back, and goes back to the version before otherwise. A bad release that runs
is replaced by publishing a newer one. Deleting a release on GitHub makes the
one before it the newest again for anyone installing; a receiver never moves
to an older version, so one that took the deleted release keeps it until a
newer one appears.
