# PhantomSDR-Plus 2.0.0 (phasor-labs, formerly Steven9101/PhantomSDR-Plus).
# Backend built from the release commit as its install.sh builds it:
#   meson build --optimization=3 && meson compile -j2 -C build
# Frontend: the release tarball's frontend/dist. The repository cannot give
# it: both frontend submodule pins (c1fdc9a at 2.0.0, 9c1daed on main) are
# gone from GitHub, and that repository's main is now NovaSDR's UI, which
# speaks a different protocol.

# Ubuntu 24.04: the README's primary platform is Ubuntu 22.04+, and Boost
# 1.83 here is below 1.87, which removed io_service APIs this code uses.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3 AS build

ARG COMMIT=b5d6eccd811b47227dbb07a6d3e5674f16ee0e4c
# subprojects/websocketpp.wrap follows websocketpp's develop branch, so each
# build takes whatever develop is that day. Pinned to develop's head on
# 2026-09-27 (commit of 2026-05-04), what an operator building today gets.
ARG WEBSOCKETPP=bce258674b2d544f981da4bf149244a76e8126c9

# install.sh's package list, with two deviations: the three Boost packages
# meson asks for (system, iostreams, headers) instead of libboost-all-dev,
# which adds some 120 packages (MPI, Python) the build does not use; and no
# cargo, which install.sh only needs for the RX888 streamer. libcurlpp-dev
# is kept: it brings the libcurl development files meson needs (libcurl).
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates build-essential cmake pkg-config meson git curl \
      libfftw3-dev libwebsocketpp-dev libflac++-dev zlib1g-dev libzstd-dev \
      libboost-dev libboost-system-dev libboost-iostreams-dev \
      libopus-dev libliquid-dev libcurlpp-dev nlohmann-json3-dev \
 && rm -rf /var/lib/apt/lists/*

RUN git init -q /src \
 && cd /src \
 && git remote add origin https://github.com/phasor-labs/PhantomSDR-Plus.git \
 && git fetch -q --depth 1 origin "$COMMIT" \
 && git checkout -q FETCH_HEAD \
 && test "$(git rev-parse HEAD)" = "$COMMIT"

WORKDIR /src
RUN grep -q '^revision = develop$' subprojects/websocketpp.wrap \
 && sed -i "s/^revision = develop$/revision = $WEBSOCKETPP/" subprojects/websocketpp.wrap \
 && grep -q "^revision = $WEBSOCKETPP$" subprojects/websocketpp.wrap

# meson.build sets b_lto_threads=0 (-flto=auto), which runs one LTO job per
# CPU at link time; the lab allows two parallel jobs, so it is set to 2.
# -march=native stays: it is the project's own flag, so the binary is built
# for this machine's CPU (AVX-512), as an operator's build would be for his.
RUN meson setup build --optimization=3 -Db_lto_threads=2 \
 && meson compile -j2 -C build \
 && test "$(cat subprojects/websocketpp-0.8.2/.git/HEAD 2>/dev/null || git -C subprojects/websocketpp-0.8.2 rev-parse HEAD)" = "$WEBSOCKETPP"

FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3 AS frontend
ARG TARBALL=https://github.com/phasor-labs/PhantomSDR-Plus/releases/download/2.0.0/phantomsdr-plus-2.0.0-opencl-linux-x86_64.tar.gz
ARG TARBALL_SHA256=f8a0d34f44812c494775c8cad727205daaec65646350cd039be04f3683f53773
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL -o /tmp/psp.tar.gz "$TARBALL" \
 && echo "$TARBALL_SHA256  /tmp/psp.tar.gz" | sha256sum -c - \
 && mkdir /out \
 && tar -xzf /tmp/psp.tar.gz -C /out --strip-components=1 \
      phantomsdr-plus-2.0.0-opencl-linux-x86_64/frontend/dist \
 && test -f /out/frontend/dist/index.html

FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libfftw3-single3 libgomp1 libboost-iostreams1.83.0 libboost-system1.83.0 \
      libzstd1 libflac++10 libliquid1 libcurl4t64 zlib1g \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/spectrumserver /opt/phantomsdr-plus/spectrumserver
# toml++ is not in the README's package list, so meson builds it from its
# wrap (3.4.0) as a shared library, as the release tarball ships it too.
COPY --from=build /src/build/subprojects/tomlplusplus-3.4.0/src/libtomlplusplus.so.3.4.0 /usr/local/lib/libtomlplusplus.so.3
COPY --from=frontend /out/frontend/dist /opt/phantomsdr-plus/frontend/dist
RUN ldconfig \
 && ln -s /opt/phantomsdr-plus/spectrumserver /usr/local/bin/spectrumserver \
 && ! ldd /opt/phantomsdr-plus/spectrumserver | grep 'not found' \
 && mkdir -p /var/lib/phantomsdr-plus

# The server writes chat_history.json and reads markers.json in its working
# directory; the harness mounts a tmpfs here. html_root in the configs is
# therefore absolute.
WORKDIR /var/lib/phantomsdr-plus
EXPOSE 9002
# It handles SIGINT (stop listening, close sockets) but not SIGTERM.
STOPSIGNAL SIGINT
# Input is stdin only (driver.name must be "stdin"); the lab's FIFO is
# redirected onto it, which is what `rtl_sdr ... | spectrumserver` gives.
CMD ["sh", "-c", "exec spectrumserver --config /etc/phantomsdr-plus/config.toml < /iq/in"]
