# PhantomSDR at the head of its default branch (b9951cb, 2024-04-06), built
# as its README says:
#   git clone --recursive https://github.com/PhantomSDR/PhantomSDR.git
#   meson build --prefer-static && meson compile -C build
# The UI is the prebuilt html-svelte/dist of the frontend submodule.

# Ubuntu 24.04: the README's first package list is Ubuntu's, and Boost 1.83
# here is below 1.87, which removed io_service APIs this code uses.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3 AS build

ARG COMMIT=b9951cbc3f5ff20854a3616a9edb7117b5ecd3f5
# subprojects/websocketpp.wrap follows websocketpp's develop branch, so each
# build takes whatever develop is that day. Pinned to develop's head on
# 2026-09-27 (commit of 2026-05-04), what an operator building today gets.
ARG WEBSOCKETPP=bce258674b2d544f981da4bf149244a76e8126c9

# The README's Ubuntu list, with two deviations: the three Boost packages
# meson asks for (system, iostreams, headers) instead of libboost-all-dev,
# which adds some 120 packages (MPI, Python) the build does not use; and
# libssl-dev, which the list lacks although meson.build requires openssl:
# without it meson builds OpenSSL 3.0.8 from a wrap instead.
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates build-essential cmake pkg-config meson git \
      libfftw3-dev libwebsocketpp-dev libflac++-dev zlib1g-dev libzstd-dev \
      libboost-dev libboost-system-dev libboost-iostreams-dev \
      libopus-dev libliquid-dev libssl-dev \
 && rm -rf /var/lib/apt/lists/*

RUN git init -q /src \
 && cd /src \
 && git remote add origin https://github.com/PhantomSDR/PhantomSDR.git \
 && git fetch -q --depth 1 origin "$COMMIT" \
 && git checkout -q FETCH_HEAD \
 && test "$(git rev-parse HEAD)" = "$COMMIT" \
 && git submodule update -q --init --depth 1 html-svelte \
 && test "$(git -C html-svelte rev-parse HEAD)" = fb595bd3027404e0edfc8fc38703d7760a76ad7c \
 && test -f html-svelte/dist/index.html

WORKDIR /src
RUN grep -q '^revision = develop$' subprojects/websocketpp.wrap \
 && sed -i "s/^revision = develop$/revision = $WEBSOCKETPP/" subprojects/websocketpp.wrap \
 && grep -q "^revision = $WEBSOCKETPP$" subprojects/websocketpp.wrap

# -j2: the lab's limit on parallel jobs (meson compile would use every CPU).
# -march=native stays: it is the project's own flag, so the binary is built
# for this machine's CPU (AVX-512), as an operator's build would be for his.
RUN meson setup build --prefer-static \
 && meson compile -j2 -C build \
 && test "$(git -C subprojects/websocketpp-0.8.2 rev-parse HEAD)" = "$WEBSOCKETPP"

FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3
# --prefer-static links FFTW, FLAC, Opus, liquid-dsp and OpenSSL statically;
# these are the shared libraries left (from ldd on the built binary).
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libboost-iostreams1.83.0 libbz2-1.0 libgomp1 liblzma5 libzstd1 zlib1g libstdc++6 libgcc-s1 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/spectrumserver /opt/phantomsdr/spectrumserver
# toml++ is not in the README's package list, so meson builds it from its
# wrap (3.4.0), as a shared library.
COPY --from=build /src/build/subprojects/tomlplusplus-3.4.0/src/libtomlplusplus.so.3.4.0 /usr/local/lib/libtomlplusplus.so.3
COPY --from=build /src/html-svelte/dist /opt/phantomsdr/html-svelte/dist
RUN ldconfig \
 && ln -s /opt/phantomsdr/spectrumserver /usr/local/bin/spectrumserver \
 && ! ldd /opt/phantomsdr/spectrumserver | grep 'not found'

# html_root "html-svelte/dist/" is relative to the working directory, as in
# the source tree the README runs it from. The server writes no files.
WORKDIR /opt/phantomsdr
EXPOSE 9002
# It handles SIGINT (stop listening, close sockets) but not SIGTERM.
STOPSIGNAL SIGINT
# Input is stdin only; the lab's FIFO is redirected onto it, which is what
# `rtl_sdr ... | spectrumserver` gives.
CMD ["sh", "-c", "exec spectrumserver --config /etc/phantomsdr/config.toml < /iq/in"]
