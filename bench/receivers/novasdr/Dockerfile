# NovaSDR 0.3.7, CPU build, as its README builds it:
#   cargo build -p novasdr-server --release [--features "soapysdr,clfft"]
# Neither feature: the lab feeds a FIFO (no SoapySDR needed) and has no GPU
# (the README: "If you want stdin-only mode, omit soapysdr. If you want a
# CPU-only build, omit clfft.").

# The toolchain the 0.3.7 release binary was built with (rustc 1.94.0 in its
# .comment section). Upstream's own Dockerfile uses a floating nightly image.
FROM rust:1.94.0-slim-bookworm@sha256:a86cada82e36ebd7a9bffed7548792c55a952fdb20718eea9278a936bcb76e62 AS build

ARG COMMIT=83495e5519c473a3b492c645eb3d54fd04baffa4

# The README's Debian package list, minus what a CPU, FIFO-input build does
# not use (SoapySDR, OpenCL/clFFT, libusb, swig/python, cmake, node/npm).
# libclang is for bindgen in crates/interop; libopus-dev for its Opus wrapper.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates git build-essential pkg-config clang libclang-dev libopus-dev \
 && rm -rf /var/lib/apt/lists/*

# The tag's commit and the frontend submodule it pins (db67b28), whose
# committed dist/ is the UI the release tarball ships (identical files).
RUN git init -q /src \
 && cd /src \
 && git remote add origin https://github.com/phasor-labs/NovaSDR.git \
 && git fetch -q --depth 1 origin "$COMMIT" \
 && git checkout -q FETCH_HEAD \
 && test "$(git rev-parse HEAD)" = "$COMMIT" \
 && git submodule update -q --init --depth 1 frontend \
 && test "$(git -C frontend rev-parse HEAD)" = db67b282ef933b0f0a923fd89f09a154f94d8517 \
 && test -f frontend/dist/index.html

# Upstream ignores lock files (*.lock in .gitignore), so every build resolves
# crates anew. This lock was generated once with rustc 1.94.0 on 2026-09-27
# and the build is held to it.
COPY Cargo.lock /src/Cargo.lock

WORKDIR /src
ENV CARGO_BUILD_JOBS=2
RUN cargo build -p novasdr-server --release --locked \
 && strip target/release/novasdr-server

FROM debian:bookworm-slim@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251

RUN apt-get update \
 && apt-get install -y --no-install-recommends libopus0 \
 && rm -rf /var/lib/apt/lists/*

# Laid out like the release tarball: the binary next to frontend/dist, which
# the default html_root "frontend/dist/" finds relative to the executable.
COPY --from=build /src/target/release/novasdr-server /opt/novasdr/novasdr-server
COPY --from=build /src/frontend/dist /opt/novasdr/frontend/dist
RUN ln -s /opt/novasdr/novasdr-server /usr/local/bin/novasdr-server \
 && mkdir -p /var/lib/novasdr

# The working directory takes what the server writes at run time: logs/
# (file logging is on by default) and chat_history.json. The harness mounts
# a tmpfs here; the config directory stays read-only.
WORKDIR /var/lib/novasdr
EXPOSE 9002
CMD ["novasdr-server", "-c", "/etc/novasdr/config.json", "-r", "/etc/novasdr/receivers.json"]
