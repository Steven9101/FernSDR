# FernSDR for the benchmark lab, built the way the release workflow builds a
# release (.github/workflows/release.yml, tools/build-release.sh): the pages
# with Node 22 and `npm ci --ignore-scripts`, the program with
# `make release ARCH=x86_64` on Ubuntu 24.04. The image then holds the
# release archive unpacked as install.sh lays it out.
#
# The build context is `git archive` of the pinned commit with this file
# added, never the working tree (NOTES.md has the command):
#
#   git -C /root/develop/websdr archive --prefix=.fernbench/ \
#       --add-file=bench/receivers/fernsdr/Dockerfile --prefix= "$COMMIT" |
#     flock /tmp/fernbench-build.lock docker build -f .fernbench/Dockerfile \
#       --build-arg COMMIT="$COMMIT" -t fernbench/fernsdr:"${COMMIT%"${COMMIT#???????}"}" -
ARG COMMIT=f70fc49f3fea348188acf45347aa73c9d5e56320

FROM node:22-bookworm-slim@sha256:43ac6c60b8f89723f746e8a92ce91abd5017e627ce1ddfe4238355d3a30b772c AS pages
WORKDIR /src/web
COPY web/package.json web/package-lock.json ./
RUN npm ci --ignore-scripts --no-audit --no-fund
COPY web/ ./
RUN npm run build

# The release job runs on GitHub's ubuntu-24.04 image; this is the same
# release of Ubuntu with only the compiler added.
FROM ubuntu:24.04@sha256:224a1869083a311ef3f13648a154ba79832fbef6364d31493642ca03082da254 AS program
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ make \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY LICENSE CHANGELOG.md ./
COPY server/ server/
COPY --from=pages /src/web/dist /pages
# The Makefile takes the archive's timestamps from `git log`, and there is no
# git here: the commit's own time (git log -1 --format=%ct) keeps the archive
# the same bytes the release script would pack. Two jobs, the lab's limit.
ARG SOURCE_DATE_EPOCH=1790505575
RUN SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH make -C server -s -j2 release ARCH=x86_64 WEB_DIST=/pages \
    && mkdir /release \
    && tar -xf server/dist/fernsdr-*-linux-x86_64.tar -C /release \
    && /release/fernsdr --version

# The program is static, so the base only has to hold a user to run it as.
FROM ubuntu:24.04@sha256:224a1869083a311ef3f13648a154ba79832fbef6364d31493642ca03082da254
ARG COMMIT
LABEL org.opencontainers.image.source="https://github.com/Steven9101/websdr" \
      org.opencontainers.image.revision="$COMMIT" \
      org.opencontainers.image.title="fernbench/fernsdr"
COPY --from=program /release /opt/fernsdr/releases/0.1.0
# As install.sh sets it up, except that the configuration is mounted read-only
# at /etc/fernsdr instead of living in /var/lib/fernsdr. The fixed uid lets
# the harness make the input FIFO readable to it.
RUN ln -s releases/0.1.0 /opt/fernsdr/current \
    && groupadd --system --gid 10001 fernsdr \
    && useradd --system --uid 10001 --gid 10001 --home-dir /var/lib/fernsdr --no-create-home \
        --shell /usr/sbin/nologin fernsdr \
    && mkdir -p /var/lib/fernsdr /etc/fernsdr \
    && chown fernsdr:fernsdr /var/lib/fernsdr
USER fernsdr
WORKDIR /var/lib/fernsdr
EXPOSE 8073
# What fernsdr.service runs (server/systemd/fernsdr.service), with the
# configuration's path changed.
CMD ["/opt/fernsdr/current/fernsdr", "/etc/fernsdr/fernsdr.conf", "--root", "/opt/fernsdr/current/web"]
