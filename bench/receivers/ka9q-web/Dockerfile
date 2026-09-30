# ka9q-radio's radiod with the ka9q-web page (wa2n-code's fork), for the
# benchmark lab, pinned as a pair: ka9q-web carries its own copies of
# radiod's status code, and radiod's preset handling changed on 2026-09-08
# with a shim added "to avoid breaking ka9q-web".
#
# Built as the projects' READMEs build them: radiod with `make` (INSTALL.md),
# libonion with the "light" cmake options (ka9q-web README, section 3A),
# ka9q-web with `make`. The lab's front end drivers (bench/drivers/ka9q)
# are built against this radiod's headers. The build context is bench/:
#
#   flock /tmp/fernbench-build.lock docker build \
#       -f bench/receivers/ka9q-web/Dockerfile -t fernbench/ka9q-web:bb5ce03-4efea4b bench
#
# Compiler flags. radiod's Makefile builds for the baseline x86-64 unless
# NATIVE=1 (since af51100, 2026-06-10); ka9q-web's hard-codes -march=native
# in COPTS. Both are built as their Makefiles do by default. The capacity
# suite's portable variant rebuilds ka9q-web with
#   --build-arg KA9Q_WEB_ARCH=-march=x86-64-v3
# and a native radiod with --build-arg RADIOD_NATIVE=1.
FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a AS build

ARG RADIOD_COMMIT=bb5ce03d089eeb74c19656844ccc8a6ab0e29a6b
ARG KA9Q_WEB_COMMIT=4efea4b58356e2ca342980f33a8c6a5b3a16693f
ARG ONION_COMMIT=de8ea938342b36c28024fd8393ebc27b8442a161
ARG RADIOD_NATIVE=0
ARG KA9Q_WEB_ARCH=-march=native

# INSTALL.md's package list, less the hardware libraries: the lab builds no
# hardware drivers (ENABLE_*=0 below), only radiod, its presets and a few
# command-line tools for checking it. libonion's README prerequisites
# (GnuTLS, libgcrypt) and cmake.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates git build-essential pkg-config cmake \
      libbsd-dev libfftw3-dev libiniparser-dev libopus-dev libogg-dev libusb-1.0-0-dev \
      libgnutls28-dev libgcrypt20-dev \
 && rm -rf /var/lib/apt/lists/*

RUN git init -q /src/ka9q-radio \
 && cd /src/ka9q-radio \
 && git remote add origin https://github.com/ka9q/ka9q-radio.git \
 && git fetch -q --depth 1 origin "$RADIOD_COMMIT" \
 && git checkout -q FETCH_HEAD \
 && test "$(git rev-parse HEAD)" = "$RADIOD_COMMIT"
# `make -j` in INSTALL.md; two jobs here, the lab's limit. Only the targets
# the lab uses: radiod plus tune, metadump, powers and pcmrecord.
RUN cd /src/ka9q-radio/src \
 && make -j2 NATIVE="$RADIOD_NATIVE" \
      ENABLE_AIRSPY=0 ENABLE_AIRSPYHF=0 ENABLE_BLADERF=0 ENABLE_FOBOS=0 ENABLE_FUNCUBE=0 \
      ENABLE_HACKRF=0 ENABLE_HYDRASDR=0 ENABLE_RTLSDR=0 ENABLE_RX888=0 ENABLE_SDRPLAY=0 \
      ENABLE_SIG_GEN=0 ENABLE_SOAPY=0 \
      radiod tune metadump powers pcmrecord \
 && install -d /out/usr/local/sbin /out/usr/local/bin /out/usr/local/lib/ka9q-radio /out/usr/local/share/ka9q-radio \
 && install -m 0755 radiod /out/usr/local/sbin/ \
 && install -m 0755 tune metadump powers pcmrecord /out/usr/local/bin/ \
 && install -m 0644 ../share/presets.conf ../share/bandplan.txt /out/usr/local/share/ka9q-radio/

COPY drivers/ka9q /src/drivers
RUN if [ "$RADIOD_NATIVE" = 1 ]; then arch=-march=native; else arch=; fi \
 && make -C /src/drivers KA9Q_SRC=/src/ka9q-radio/src ARCHOPTS="$arch" \
 && install -m 0755 /src/drivers/rtltcp.so /src/drivers/fifos16.so /out/usr/local/lib/ka9q-radio/

RUN git init -q /src/onion \
 && cd /src/onion \
 && git remote add origin https://github.com/davidmoreno/onion.git \
 && git fetch -q --depth 1 origin "$ONION_COMMIT" \
 && git checkout -q FETCH_HEAD \
 && test "$(git rev-parse HEAD)" = "$ONION_COMMIT" \
 && mkdir build && cd build \
 && cmake -DONION_USE_PAM=false -DONION_USE_PNG=false -DONION_USE_JPEG=false -DONION_USE_XML2=false \
      -DONION_USE_SYSTEMD=false -DONION_USE_SQLITE3=false -DONION_USE_REDIS=false -DONION_USE_GC=false \
      -DONION_USE_TESTS=false -DONION_EXAMPLES=false -DONION_USE_BINDINGS_CPP=false .. > cmake.log \
 && grep -q 'SSL support is compiled in' cmake.log \
 && make -j2 \
 && make install \
 && ldconfig \
 && cp -a /usr/local/lib/libonion.so* /out/usr/local/lib/

# ka9q-web's Makefile takes GIT_COMMIT and the commit count from git, so the
# clone keeps its history (the count is ka9q-web's own version index).
RUN git clone -q https://github.com/wa2n-code/ka9q-web.git /src/ka9q-web \
 && cd /src/ka9q-web \
 && git checkout -q "$KA9Q_WEB_COMMIT" \
 && test "$(git rev-parse HEAD)" = "$KA9Q_WEB_COMMIT" \
 && make -j2 COPTS="$KA9Q_WEB_ARCH -std=gnu11 -pthread -Wall -funsafe-math-optimizations -D_GNU_SOURCE=1" \
 && install -d /out/usr/local/sbin /out/usr/local/share/ka9q-web/html \
 && install -m 0755 ka9q-web /out/usr/local/sbin/ \
 && install -m 0644 html/* /out/usr/local/share/ka9q-web/html/

FROM debian:trixie-slim@sha256:a99cfc517144bc59b1978475ec53b46ecabec7e43635402ee5b77cc54cd1b20a
# Runtime libraries of the above; iproute2 for the loopback multicast flag.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libbsd0 libfftw3-single3 libiniparser4 libopus0 libogg0 libusb-1.0-0 \
      libgnutls30t64 libgcrypt20 iproute2 \
 && rm -rf /var/lib/apt/lists/* \
 && install -d /var/lib/ka9q-radio /etc/radio /etc/fftw
COPY --from=build /out/ /
COPY receivers/ka9q-web/lab-start /usr/local/bin/lab-start
# Fails the build if a library is missing.
RUN ldconfig \
 && ! ldd /usr/local/sbin/radiod /usr/local/sbin/ka9q-web /usr/local/lib/ka9q-radio/*.so \
      /usr/local/bin/tune /usr/local/bin/metadump /usr/local/bin/powers /usr/local/bin/pcmrecord | grep 'not found'
EXPOSE 8081
ENTRYPOINT ["/usr/local/bin/lab-start"]
CMD ["/etc/fernbench/radiod.conf", "-m", "239.201.1.1,lo", "-p", "8081"]
