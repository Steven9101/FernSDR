# VertexSDR for the benchmark lab, built as scripts/setup.sh does on Ubuntu
# 24.04: its apt packages, `make`, then scripts/fetch-frontend.sh for the
# page files. Build (NOTES.md):
#
#   flock /tmp/fernbench-build.lock docker build \
#     -t fernbench/vertexsdr:a78a943 bench/receivers/vertexsdr
ARG COMMIT=a78a943229b59d16348ffcaa4a8e29158ea2c4fd

FROM ubuntu:24.04@sha256:224a1869083a311ef3f13648a154ba79832fbef6364d31493642ca03082da254 AS build
ARG COMMIT
# setup.sh's list, plus git and CA certificates to fetch the source and the
# page files over HTTPS; setup.sh runs inside a clone, with sudo, on a
# system whose package lists are already current.
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential libfftw3-dev libpng-dev libasound2-dev \
        libssl-dev xxd patch curl ca-certificates git \
    && rm -rf /var/lib/apt/lists/*
RUN git clone https://github.com/Steven9101/VertexSDR /src \
    && git -C /src checkout --detach "$COMMIT" \
    && test "$(git -C /src rev-parse HEAD)" = "$COMMIT"
WORKDIR /src
# setup.sh runs plain `make`; two jobs is the lab's limit and changes nothing
# in the result.
RUN make -j2 && test -x vertexsdr
# The page files are PA3FWM's, fetched once here from third-party operators'
# repositories (their main and master branches) and patched by the script.
# A patch that does not apply is only a warning in the script, which would
# leave a page without VertexSDR's changes, so a warning fails the build.
RUN bash scripts/fetch-frontend.sh > /tmp/fetch.log 2>&1; status=$?; cat /tmp/fetch.log; \
    test "$status" -eq 0 && ! grep -q 'WARNING\|ERROR' /tmp/fetch.log
# What the lab tested on 2026-09-27 (NOTES.md lists the files as fetched,
# before patching): a changed file upstream fails the build instead of
# silently changing the receiver.
RUN cd pub2 && printf '%s\n' \
    '7d8983675c032c2a68770ee0096344eb8dea1ae9f50e559710647e872a0bebb2  carrier.png' \
    '8935a081327c9c81925001c9946a24b1cedd2c0da381b9384186cf37fdb1b7bd  crossdomain.xml' \
    'c801af81296710f38c634ffb418feedd2317bb84e05ee28d57de8d38515f144f  edgelower.png' \
    'c10e7b49d24e0563e67468b2d9e4892a5c0c0a521763d78855122681c4067ef4  edgelowerbb.png' \
    '5c10d68645f103e235ae13bb8d864ee75cadfb8820e646aaa7cadce783494adb  edgeupper.png' \
    '6c8418b70342d99f5669103dde3a2bf44a137a2a78dfc7a515380987ad0423d5  edgeupperbb.png' \
    'db854a00528e3b5aa65abe50a930a76c535f2ea087bec53e5861252d0ed02283  index.html' \
    'c11a75aad8f8c70b7b4970d55a9a718ee017bd1017d13f0bb1499584ba5d0257  logbook.txt' \
    '0093c0486a40a8db18546282b4eceb9eb140e52ab112272f7dd1fab14b6e0a81  m.html' \
    '25c9540b2a983062bb33b01e7ea1ecdee2edcf94eb431d7c0ab236bd60fb84ba  mobile-controls.html' \
    'fe08b5c428f4076261e64e3b675dad67f1a3ff5fd7c192f823ff1aba42050e8e  scaleblack.png' \
    '8b5fc0c0684b6ce43baa70ae2bbfec0a940e103d0adb94070f4103b688b4cf1e  smeter1.png' \
    '17f9a4c7219810f3f9c515d7e7bf1ecc96c2c62aa8a728cbb396eec8fc021ea8  sysop.html' \
    'b3c83f4a3ee3df2822ee6efaea5f420efbb1820bc8881042b61ea310d4ac2cbd  websdr-1405020937.jar' \
    '5e7357f17b19addd86a680fcb7669ed1ea1cc3292a21cb8abc2da5b4691d80ed  websdr-base.js' \
    'f66be46f068b9b89f4ed519b499bbe88ddddbd6d79e13359623cfecd7693a08f  websdr-controls.html' \
    'c9ec464a5180f4d294e3862857115f97109e722bc0c625db8ec673358e5cc802  websdr-head.html' \
    'f3363d6eedd10017eba895bce405d4aa4c3518c1212925708918cb945e972cae  websdr-javasound.js' \
    '742719e8dd2a882347839008882c1e26c319474dd823ac05fc31f5e0ca588359  websdr-javawaterfall.js' \
    '69c5131a1b2371dfed20901b13d11b7c5fac1df5065a3926daec031795d51c11  websdr-sound.js' \
    '507842c5d6632360b46c39ba16d970ccad340b2d2167781ae47717cd381a35e0  websdr-waterfall.js' \
    | sha256sum -c --strict - \
    && test "$(ls | wc -l)" -eq 21

FROM ubuntu:24.04@sha256:224a1869083a311ef3f13648a154ba79832fbef6364d31493642ca03082da254
ARG COMMIT
LABEL org.opencontainers.image.source="https://github.com/Steven9101/VertexSDR" \
      org.opencontainers.image.revision="$COMMIT" \
      org.opencontainers.image.title="fernbench/vertexsdr"
# The run-time halves of setup.sh's -dev packages.
RUN apt-get update \
    && apt-get install -y --no-install-recommends libfftw3-single3 libpng16-16t64 libasound2t64 libssl3t64 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/vertexsdr /opt/vertexsdr/vertexsdr
COPY --from=build /src/pub2 /opt/vertexsdr/pub2
# VertexSDR ends at start when nothing listens on its rtl_tcp address
# (src/main.c, "Audio input startup failed"); the README has the operator
# start rtl_tcp first. The lab's pace joins this container's network
# namespace only once the container exists, so this waits, without
# connecting, until something listens on 127.0.0.1:1234, then runs the
# command. /proc/net/tcp holds that address as 0100007F:04D2, state 0A.
RUN printf '%s\n' '#!/bin/sh' \
      'i=0' \
      'until grep -q " 0100007F:04D2 00000000:0000 0A " /proc/net/tcp; do' \
      '  i=$((i + 1))' \
      '  if [ "$i" -gt 1200 ]; then echo "nothing listens on 127.0.0.1:1234 after 120 s" >&2; exit 1; fi' \
      '  sleep 0.1' \
      'done' \
      'exec "$@"' > /opt/vertexsdr/wait-rtltcp \
    && chmod 755 /opt/vertexsdr/wait-rtltcp \
    && groupadd --system --gid 10001 vertexsdr \
    && useradd --system --uid 10001 --gid 10001 --home-dir /opt/vertexsdr --no-create-home \
        --shell /usr/sbin/nologin vertexsdr \
    && mkdir -p /etc/vertexsdr /opt/vertexsdr/pub2/tmp \
    && chmod 1777 /opt/vertexsdr/pub2/tmp
USER vertexsdr
# `public pub2` in the configuration is relative to here. At start the
# program writes pub2/tmp/bandinfo.js, which the page loads, and label
# images beside it: under --read-only that directory must be a tmpfs, and
# Docker gives a tmpfs the mode of the directory it covers, hence 1777
# above. It also saves fftw-wisdom.dat here, and goes on without it when
# this is read-only.
WORKDIR /opt/vertexsdr
EXPOSE 8901
CMD ["/opt/vertexsdr/wait-rtltcp", "/opt/vertexsdr/vertexsdr", "/etc/vertexsdr/websdr.cfg"]
