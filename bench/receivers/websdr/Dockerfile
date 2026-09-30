# PA3FWM's WebSDR server for the benchmark lab: the dist11 x86_64 binary
# (websdr64, "20140718.1716-64") from the copy at github.com/py1tcm/websdr,
# used with PA3FWM's permission (NOTES.md), laid out as dist11/doc/install.txt
# describes. Build (NOTES.md):
#
#   flock /tmp/fernbench-build.lock docker build \
#     -t fernbench/websdr:64dd6b9 bench/receivers/websdr
ARG COMMIT=64dd6b9c3d6c2cf102999a53bb8b4e1b32528840

FROM ubuntu:24.04@sha256:224a1869083a311ef3f13648a154ba79832fbef6364d31493642ca03082da254 AS fetch
ARG COMMIT
RUN apt-get update \
    && apt-get install -y --no-install-recommends git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
RUN git clone https://github.com/py1tcm/websdr /src \
    && git -C /src checkout --detach "$COMMIT" \
    && test "$(git -C /src rev-parse HEAD)" = "$COMMIT"
# The binary the lab tested; /root/websdr64/websdr64 on the lab machine has
# the same hash.
RUN echo 'c8ad69c59976c3171011818daf000fffe81d5b375ddf6abb2d559eba9e54cc80  /src/dist11/websdr64' | sha256sum -c -
# install.txt's layout in /home/websdr: dist11 (program, documentation,
# pub2), pub (the site's pages: copies of pub2's index.html and m.html,
# steps 5 and 6) and cfg (the configuration, mounted at run time, step 4).
# Left out of the copy: the station's logs (log/, userslog.txt and
# log-cpuload.txt hold its listeners' addresses), its FFTW wisdom (made for
# its machine), its pub/ (its own site pages), the 32-bit and Raspberry Pi
# binaries, and the bundled libcrypto.so.1.0.0 and libpng12.so.0, which the
# Ubuntu 16.04 packages below provide instead. pub2/tmp and log are where the
# server writes; their mode carries over to the tmpfs mounted on them. The
# empty logbook is one the server otherwise creates at start in ../pub: on
# a read-only pub it says it created one and then dies of SIGSEGV
# (NOTES.md).
RUN d=/out/home/websdr \
    && mkdir -p "$d/dist11/pub2" "$d/pub" "$d/cfg" \
    && cd /src/dist11 \
    && cp -a websdr64 websdr.cfg cty.dat doc "$d/dist11/" \
    && for f in pub2/*; do [ "$f" = pub2/tmp ] || cp -a "$f" "$d/dist11/pub2/"; done \
    && mkdir -m 1777 "$d/dist11/pub2/tmp" "$d/dist11/log" \
    && cp pub2/index.html pub2/m.html "$d/pub/" \
    && touch "$d/pub/logbook.txt"

# dist11 needs libfftw3f, libpng12 and libcrypto 1.0.0 (plus ALSA and zlib).
# Ubuntu 16.04 is the newest Ubuntu that still packages libpng12-0 and
# libssl1.0.0; its packages are what install.txt names (libfftw3,
# libpng12-0), so nothing from the copy's bundled libraries is needed.
FROM ubuntu:16.04@sha256:1f1a2d56de1d604801a9671f301190704c25d604a416f59e03c04f5c6ffee0d6
ARG COMMIT
LABEL org.opencontainers.image.source="https://github.com/py1tcm/websdr" \
      org.opencontainers.image.revision="$COMMIT" \
      org.opencontainers.image.title="fernbench/websdr"
RUN apt-get update \
    && apt-get install -y --no-install-recommends libfftw3-single3 libpng12-0 libssl1.0.0 libasound2 zlib1g \
    && rm -rf /var/lib/apt/lists/*
# Step 1: an account of its own.
RUN groupadd --gid 10001 websdr \
    && useradd --uid 10001 --gid 10001 --home-dir /home/websdr --no-create-home --shell /usr/sbin/nologin websdr
COPY --from=fetch /out/home/websdr /home/websdr
# The server ends at start when nothing listens on its rtl_tcp address
# ("Can't connect to RTL-SDR driver at 127.0.0.1:1234"); the copy's own
# systemd unit orders rtl_tcp first and restarts it. The lab's pace joins
# this container's network namespace only once the container exists, so
# this waits, without connecting, until something listens on
# 127.0.0.1:1234, then runs the command. /proc/net/tcp holds that address
# as 0100007F:04D2, state 0A.
RUN printf '%s\n' '#!/bin/sh' \
      'i=0' \
      'until grep -q " 0100007F:04D2 00000000:0000 0A " /proc/net/tcp; do' \
      '  i=$((i + 1))' \
      '  if [ "$i" -gt 1200 ]; then echo "nothing listens on 127.0.0.1:1234 after 120 s" >&2; exit 1; fi' \
      '  sleep 0.1' \
      'done' \
      'exec "$@"' > /usr/local/bin/wait-rtltcp \
    && chmod 755 /usr/local/bin/wait-rtltcp
USER websdr
# Step 7: started from dist11. It reads ../cfg/websdr.cfg and serves ../pub
# and pub2.
WORKDIR /home/websdr/dist11
EXPOSE 8901
CMD ["/usr/local/bin/wait-rtltcp", "./websdr64"]
