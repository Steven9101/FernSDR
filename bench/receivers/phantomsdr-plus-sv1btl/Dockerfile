# PhantomSDR-Plus as maintained by SV1BTL (README "version 4.2.0"), at the
# head of its default branch. Its README installs with ./install.sh, an
# interactive script that also sets up an SDR driver, OpenCL, an admin panel,
# a RADE decoder and a statistics server. Only the receiver is built here,
# with the installer's own steps for it:
#   backend:  apt packages; meson setup build; copy the five patched
#             websocketpp headers over the subproject; meson compile -j2
#   frontend: npm install (a fixed sequence); ./build-all.sh
# No admin panel, proxy, statistics server, decoders' autorun or thermal
# guard: the lab measures the receiver.

ARG COMMIT=0fb2ee03fade61b71a4e93554cb28ccb32e8e7f5

# Node 22: the installer puts Node.js 22 in place through nvm.
FROM node:22-bookworm-slim@sha256:43ac6c60b8f89723f746e8a92ce91abd5017e627ce1ddfe4238355d3a30b772c AS frontend
ARG COMMIT
# python3 runs fix-title-python.py at the end of build-all.sh.
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates git python3 \
 && rm -rf /var/lib/apt/lists/*
RUN git init -q /src \
 && cd /src \
 && git remote add origin https://github.com/sv1btl/PhantomSDR-Plus.git \
 && git fetch -q --depth 1 origin "$COMMIT" \
 && git checkout -q FETCH_HEAD \
 && test "$(git rev-parse HEAD)" = "$COMMIT"
# The installer deletes the committed package-lock.json and resolves anew
# (npm install of pinned Vite/Svelte packages, then of package.json, then
# of three more packages, then npm audit fix), so each install gets that
# day's versions. That sequence was run once on 2026-09-27 (Node 22.23.3,
# npm 10.9.9); its package.json (socket.io-client ^4.8.3 -> ^4.8.4) and
# lock are frontend/ here, and the build is held to them with npm ci.
COPY frontend/package.json frontend/package-lock.json /src/frontend/
WORKDIR /src/frontend
RUN npm ci --no-audit --no-fund
# One Vite build at a time (build-all.sh runs up to three) and a bounded
# heap: the machine this is built on is shared.
ENV PHANTOM_BUILD_JOBS=1 NODE_OPTIONS=--max-old-space-size=1536
# The script is not executable in git; the installer chmods it first too.
RUN chmod +x build-all.sh \
 && ./build-all.sh \
 && test -f dist/index.html && test -f dist/mobile/index.html

# Ubuntu 24.04, one of the installer's tested systems; Boost 1.83 there is
# below 1.87, so the installer copies the websocketpp patches but does not
# require the Boost-1.87 part of them.
FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3 AS build
ARG COMMIT
# subprojects/websocketpp.wrap follows websocketpp's develop branch, so each
# build takes whatever develop is that day. Pinned to develop's head on
# 2026-09-27 (commit of 2026-05-04), what an operator building today gets.
ARG WEBSOCKETPP=bce258674b2d544f981da4bf149244a76e8126c9

# install.sh's two package lists (prerequisites and build dependencies),
# with libboost-dev, libboost-system-dev and libboost-iostreams-dev instead
# of libboost-all-dev (some 120 packages more: MPI, Python) since meson asks
# for system and iostreams only; and without libusb, psmisc, procps, nano,
# util-linux and findutils, which serve the SDR drivers and the scripts.
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates curl build-essential cmake pkg-config meson ninja-build git python3 \
      libfftw3-dev libwebsocketpp-dev libflac++-dev zlib1g-dev libzstd-dev \
      libboost-dev libboost-system-dev libboost-iostreams-dev \
      libopus-dev libliquid-dev libcurl4-openssl-dev nlohmann-json3-dev \
 && rm -rf /var/lib/apt/lists/*

RUN git init -q /src \
 && cd /src \
 && git remote add origin https://github.com/sv1btl/PhantomSDR-Plus.git \
 && git fetch -q --depth 1 origin "$COMMIT" \
 && git checkout -q FETCH_HEAD \
 && test "$(git rev-parse HEAD)" = "$COMMIT"

WORKDIR /src
RUN grep -q '^revision = develop$' subprojects/websocketpp.wrap \
 && sed -i "s/^revision = develop$/revision = $WEBSOCKETPP/" subprojects/websocketpp.wrap \
 && grep -q "^revision = $WEBSOCKETPP$" subprojects/websocketpp.wrap

# install.sh step 6: meson setup, then patch_websocketpp (the five headers
# from the repository root over the fetched subproject, then its checks),
# then meson compile -j2.
RUN meson setup build \
 && test "$(git -C subprojects/websocketpp-0.8.2 rev-parse HEAD)" = "$WEBSOCKETPP" \
 && W=subprojects/websocketpp-0.8.2 \
 && cp request.hpp                     $W/websocketpp/http/impl/request.hpp \
 && cp connection_impl.hpp             $W/websocketpp/impl/connection_impl.hpp \
 && cp websocketpp_asio.hpp            $W/websocketpp/common/asio.hpp \
 && cp websocketpp_asio_connection.hpp $W/websocketpp/transport/asio/connection.hpp \
 && cp websocketpp_asio_endpoint.hpp   $W/websocketpp/transport/asio/endpoint.hpp \
 && grep -q io_context            $W/websocketpp/common/asio.hpp \
 && grep -q ws_max_listen_backlog $W/websocketpp/common/asio.hpp \
 && grep -q ws_work               $W/websocketpp/transport/asio/endpoint.hpp \
 && grep -q ws_post               $W/websocketpp/transport/asio/connection.hpp \
 && meson compile -j2 -C build

FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libfftw3-single3 libgomp1 libboost-iostreams1.83.0 libboost-system1.83.0 \
      libzstd1 libflac++10 libopus0 libliquid1 libcurl4t64 zlib1g ca-certificates \
 && rm -rf /var/lib/apt/lists/*
ENV APP=/opt/phantomsdr-plus-sv1btl
COPY --from=build /src/build/spectrumserver $APP/spectrumserver
# toml++ comes from the subproject the repository vendors (3.4.0), built as
# a shared library.
COPY --from=build /src/build/subprojects/tomlplusplus-3.4.0/src/libtomlplusplus.so.3.4.0 /usr/local/lib/libtomlplusplus.so.3
COPY --from=frontend /src/frontend/dist $APP/frontend/dist
# What an installed receiver's working directory holds at the start: the
# repository's markers.json, empty chat_history.txt and FFTW wisdom stub.
COPY --from=build /src/markers.json /src/chat_history.txt /src/phantom_fftw_wisdom $APP/seed/
RUN ldconfig \
 && ln -s $APP/spectrumserver /usr/local/bin/spectrumserver \
 && ! ldd $APP/spectrumserver | grep 'not found' \
 && mkdir -p /var/lib/phantomsdr-plus-sv1btl

# The server writes into its working directory (chat_history.txt,
# crash.log, phantom_fftw_wisdom) and into its html root (users.json and
# logs/users_<date>.jsonl, next to the UI). The harness mounts a tmpfs on
# /var/lib/phantomsdr-plus-sv1btl, so the start copies the seed files and
# the UI there, and html_root in the configs points at that copy.
WORKDIR /var/lib/phantomsdr-plus-sv1btl
EXPOSE 9002
# Input is stdin only; the lab's FIFO is redirected onto it, as the
# project's own start-*.sh scripts do with their FIFO.
CMD ["sh", "-c", "cp -a $APP/seed/. . && mkdir -p www && cp -a $APP/frontend/dist/. www/ && exec spectrumserver --config /etc/phantomsdr-plus-sv1btl/config.toml < /iq/in"]
