# Plain make: no cmake, no autotools, no external libraries.  `make` builds the
# server, `make test` builds and runs the unit tests.

CXX      ?= g++
CXXSTD   ?= -std=c++17
OPT      ?= -O3
WARN     := -Wall -Wextra -Wno-unused-parameter
# TARGET_FLAGS and LINK_FLAGS are for release builds (see `release` below),
# which set them on a make of their own rather than replace CXXFLAGS.
# Large files everywhere: a month's waterfall archive is past 2 GB, which a
# 32-bit system (armhf) otherwise cannot open or seek in.
CXXFLAGS += $(CXXSTD) $(OPT) $(WARN) $(TARGET_FLAGS) -D_FILE_OFFSET_BITS=64 -pthread -MMD -MP
LDFLAGS  += $(TARGET_FLAGS) $(LINK_FLAGS) -pthread
LDLIBS   += -lm

BUILD    := build
SRC_DIR  := src
TEST_DIR := tests

SERVER_SRCS := $(shell find $(SRC_DIR) -name '*.cpp')
TOOL_SRCS   := $(wildcard tools/*.cpp)
# tests/support holds programs the tests run, each with its own main().
TEST_SRCS   := $(shell find $(TEST_DIR) -name '*.cpp' -not -path '$(TEST_DIR)/support/*')
# The test binary links the whole server except its entry point.
LIB_SRCS    := $(filter-out $(SRC_DIR)/main.cpp,$(SERVER_SRCS))

SERVER_OBJS := $(SERVER_SRCS:%.cpp=$(BUILD)/%.o)
TEST_OBJS   := $(TEST_SRCS:%.cpp=$(BUILD)/%.o) $(LIB_SRCS:%.cpp=$(BUILD)/%.o)

SERVER_BIN := $(BUILD)/fernsdr
TEST_BIN   := $(BUILD)/fernsdr-tests

.PHONY: all test clean run fast vectors bench archive-bench quality roundtrip lab fft-bench fft-compare link-model agc-lab wideband-module fuzz ed25519-compare release-tools release release-binary release-test installer docker

all: $(SERVER_BIN)

# Keep the old entry point, but retain IEEE checks in parsers and DSP guards.
# -ffast-math can compile isfinite() away and breaks the input validation.
fast: all

$(SERVER_BIN): $(SERVER_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(TEST_BIN): $(TEST_OBJS) | $(BUILD)/fake-module $(BUILD)/fake-decoder $(BUILD)/fernsdr
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $(TEST_OBJS) $(LDLIBS)

# The decoder the decoder tests start, and the receiver itself, whose
# --sandbox-exec they start it through.
$(BUILD)/fake-decoder: $(BUILD)/tests/support/fake_decoder.o
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# The module the module tests start: a real program, beside the test binary.
$(BUILD)/fake-module: $(BUILD)/tests/support/fake_module.o
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# A machine runs only its widest transforms: repeat the tests that use them
# with each narrower instruction set (a lowering the CPU cannot take changes
# nothing).
test: $(TEST_BIN)
	@$(TEST_BIN)
	@for isa in avx2 sse2 scalar; do for part in fft mdct channel spectrum simd rds_ band_decimator ctcss; do \
		out=$$(FERNSDR_FFT_ISA=$$isa $(TEST_BIN) $$part) || { echo "$$out" | grep FAIL; exit 1; }; \
		echo "FERNSDR_FFT_ISA=$$isa, $$part tests: $$(echo "$$out" | tail -n 1)"; \
	done; done

# Regenerates the cross-check vectors the client's decoder tests run against.
vectors: $(BUILD)/gen-vectors
	@mkdir -p ../web/src/dsp/testdata
	@$(BUILD)/gen-vectors ../web/src/dsp/testdata/vectors.json

$(BUILD)/gen-vectors: $(BUILD)/tools/gen_vectors.o $(LIB_SRCS:%.cpp=$(BUILD)/%.o)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

quality: $(BUILD)/audio-quality
	@$(BUILD)/audio-quality

$(BUILD)/audio-quality: $(BUILD)/tools/audio_quality.o $(LIB_SRCS:%.cpp=$(BUILD)/%.o)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

roundtrip: $(BUILD)/nac-roundtrip

$(BUILD)/nac-roundtrip: $(BUILD)/tools/nac_roundtrip.o $(LIB_SRCS:%.cpp=$(BUILD)/%.o)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Codec measurements on real recordings; see tools/codec_lab.cpp.
lab: $(BUILD)/codec-lab

$(BUILD)/codec-lab: $(BUILD)/tools/codec_lab.o $(LIB_SRCS:%.cpp=$(BUILD)/%.o)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# libFuzzer targets for what an untrusted peer or file reaches first, built with
# clang, AddressSanitizer and UndefinedBehaviorSanitizer into build-fuzz/. New
# inputs go to the first directory given, so the tracked seeds come second:
#   make fuzz && mkdir -p /tmp/json && build-fuzz/fuzz-json -max_total_time=600 /tmp/json fuzz/corpus/json
FUZZ_CXX   ?= clang++
FUZZ_FLAGS := $(CXXSTD) -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
FUZZ_BUILD := build-fuzz

fuzz: $(FUZZ_BUILD)/fuzz-http $(FUZZ_BUILD)/fuzz-websocket $(FUZZ_BUILD)/fuzz-json $(FUZZ_BUILD)/fuzz-config \
      $(FUZZ_BUILD)/fuzz-module-package $(FUZZ_BUILD)/fuzz-ed25519 $(FUZZ_BUILD)/fuzz-ustar $(FUZZ_BUILD)/fuzz-release

$(FUZZ_BUILD)/fuzz-http: fuzz/fuzz_http.cpp src/net/http.cpp src/util/config.cpp
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_FLAGS) -o $@ $^

$(FUZZ_BUILD)/fuzz-websocket: fuzz/fuzz_websocket.cpp src/net/websocket.cpp
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_FLAGS) -o $@ $^

$(FUZZ_BUILD)/fuzz-json: fuzz/fuzz_json.cpp src/util/json.cpp
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_FLAGS) -o $@ $^

$(FUZZ_BUILD)/fuzz-config: fuzz/fuzz_config.cpp src/util/config.cpp
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_FLAGS) -o $@ $^

$(FUZZ_BUILD)/fuzz-module-package: fuzz/fuzz_module_package.cpp src/core/module_store.cpp src/util/json.cpp \
                                   src/util/password.cpp src/util/config.cpp
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_FLAGS) -o $@ $^

$(FUZZ_BUILD)/fuzz-ed25519: fuzz/fuzz_ed25519.cpp src/util/ed25519.cpp
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_FLAGS) -o $@ $^

$(FUZZ_BUILD)/fuzz-ustar: fuzz/fuzz_ustar.cpp src/update/ustar.cpp
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_FLAGS) -o $@ $^

$(FUZZ_BUILD)/fuzz-release: fuzz/fuzz_release.cpp src/update/release.cpp src/util/ed25519.cpp
	@mkdir -p $(dir $@)
	$(FUZZ_CXX) $(FUZZ_FLAGS) -o $@ $^

# Release keys, manifests and signatures; see tools/fernsdr_release.cpp.
release-tools: $(BUILD)/fernsdr-release

$(BUILD)/fernsdr-release: $(BUILD)/tools/fernsdr_release.o $(LIB_SRCS:%.cpp=$(BUILD)/%.o)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Release builds, one platform each, in build-release-ARCH/:
#   make release ARCH=x86_64|aarch64|armhf   dist/fernsdr-VERSION-linux-ARCH.tar
#   make release-test ARCH=...                the tests, under qemu-user for ARM
# The program is static and stripped, so it runs on any Linux of its
# architecture without a library from the system. armhf is ARMv7 with NEON
# and hardware floating point: a Raspberry Pi 2 or later running a 32-bit
# system. glibc here has no static PIE for 32-bit ARM, so that one is plain
# static. WEB_DIST is the pages' build, made beforehand with npm run build.
ARCH ?= x86_64
RELEASE_CXX_x86_64  := g++
RELEASE_CXX_aarch64 := aarch64-linux-gnu-g++
RELEASE_CXX_armhf   := arm-linux-gnueabihf-g++
RELEASE_TARGET_armhf := -march=armv7-a -mfpu=neon -mfloat-abi=hard
RELEASE_LINK_x86_64  := -static-pie
RELEASE_LINK_aarch64 := -static-pie
RELEASE_LINK_armhf   := -static
RELEASE_RUN_aarch64 := FERNSDR_TEST_UNDER_QEMU=1 qemu-aarch64 -cpu cortex-a53
RELEASE_RUN_armhf   := FERNSDR_TEST_UNDER_QEMU=1 qemu-arm -cpu cortex-a7
RELEASE_ISAS_x86_64  := avx2 sse2 scalar
RELEASE_ISAS_aarch64 := scalar
RELEASE_ISAS_armhf   := scalar
RELEASE_BUILD := build-release-$(ARCH)
RELEASE_MAKE = $(MAKE) --no-print-directory BUILD=$(RELEASE_BUILD) CXX=$(RELEASE_CXX_$(ARCH)) \
               TARGET_FLAGS="$(RELEASE_TARGET_$(ARCH))" LINK_FLAGS="$(RELEASE_LINK_$(ARCH)) -s"
VERSION := $(shell sed -n 's/.*kVersion = "\(.*\)";/\1/p' src/version.h)
WEB_DIST ?= ../web/dist
RELEASE_TAR := dist/fernsdr-$(VERSION)-linux-$(ARCH).tar

release-binary:
	@test -n "$(RELEASE_CXX_$(ARCH))" || { echo "ARCH is x86_64, aarch64 or armhf, not $(ARCH)"; exit 1; }
	@+$(RELEASE_MAKE) $(RELEASE_BUILD)/fernsdr

# The tests run as the target runs them. The fake module and decoder they
# start are the host's own builds: qemu-user hands exec to the host kernel,
# which could not run ARM ones, and the module protocols are the same on
# every architecture.
release-test: $(BUILD)/fake-module $(BUILD)/fake-decoder
	@test -n "$(RELEASE_CXX_$(ARCH))" || { echo "ARCH is x86_64, aarch64 or armhf, not $(ARCH)"; exit 1; }
	@+$(RELEASE_MAKE) $(RELEASE_BUILD)/fernsdr-tests
	@cp $(BUILD)/fake-module $(RELEASE_BUILD)/fake-module
	@cp $(BUILD)/fake-decoder $(RELEASE_BUILD)/fake-decoder
	@$(RELEASE_RUN_$(ARCH)) $(RELEASE_BUILD)/fernsdr-tests
	@for isa in $(RELEASE_ISAS_$(ARCH)); do for part in fft mdct channel spectrum simd rds_ band_decimator ctcss; do \
		out=$$(FERNSDR_FFT_ISA=$$isa $(RELEASE_RUN_$(ARCH)) $(RELEASE_BUILD)/fernsdr-tests $$part) || \
			{ echo "$$out" | grep FAIL; exit 1; }; \
		echo "$(ARCH), FERNSDR_FFT_ISA=$$isa, $$part tests: $$(echo "$$out" | tail -n 1)"; \
	done; done

# The archive holds the program, the pages, an example configuration, the
# systemd units install.sh sets up and the scripts it uses where there is no
# systemd, the licence and a notice for the
# libraries linked into the program.
release: release-binary $(BUILD)/fernsdr-release
	@test -f $(WEB_DIST)/index.html || { echo "no pages in $(WEB_DIST): build them first"; exit 1; }
	@rm -rf $(RELEASE_BUILD)/tree
	@mkdir -p $(RELEASE_BUILD)/tree/systemd dist
	cp $(RELEASE_BUILD)/fernsdr fernsdr.example.conf ../LICENSE $(RELEASE_BUILD)/tree/
	cp tools/release-notice.txt $(RELEASE_BUILD)/tree/NOTICE
	cp systemd/*.service systemd/*.path $(RELEASE_BUILD)/tree/systemd/
	cp -R init $(RELEASE_BUILD)/tree/init
	cp -R $(WEB_DIST) $(RELEASE_BUILD)/tree/web
	SOURCE_DATE_EPOCH=$${SOURCE_DATE_EPOCH:-$$(git log -1 --format=%ct 2>/dev/null || echo 0)} \
		$(BUILD)/fernsdr-release pack $(RELEASE_TAR) $(RELEASE_BUILD)/tree
	@echo "$(RELEASE_TAR): $$(wc -c < $(RELEASE_TAR)) bytes"

# A container image from the release's tree: docker/Dockerfile on Alpine,
# tagged fernsdr:VERSION. Local builds are for this machine's processor;
# docker buildx makes the others from their trees the same way.
docker: release
	@rm -rf $(RELEASE_BUILD)/docker
	@mkdir -p $(RELEASE_BUILD)/docker
	cp -R $(RELEASE_BUILD)/tree $(RELEASE_BUILD)/docker/tree
	cp docker/Dockerfile docker/entrypoint.sh $(RELEASE_BUILD)/docker/
	docker build -t fernsdr:$(VERSION) $(RELEASE_BUILD)/docker

# dist/install.sh, published beside the archives: tools/install.sh with the
# keys this build's receivers check releases with, so that the first install
# is checked with the keys every later update is. Without a key there is
# nothing to check with, and no installer.
installer: $(BUILD)/fernsdr-release
	@mkdir -p dist
	@keys=$$($(BUILD)/fernsdr-release public-keys | tr '\n' ' ' | sed 's/ $$//') && \
		test -n "$$keys" || { echo "src/update/release_keys.h carries no key, so there is no installer"; exit 1; }; \
		sed "s|@RELEASE_KEYS@|$$keys|" ../tools/install.sh > dist/install.sh.new && \
		! grep -q '@RELEASE_KEYS@' dist/install.sh.new && chmod 755 dist/install.sh.new && \
		mv dist/install.sh.new dist/install.sh
	@echo "dist/install.sh: $$(grep -c '^' dist/install.sh) lines"

# Ed25519 against OpenSSL; see tools/ed25519_compare.cpp. libcrypto is linked
# into that tool only, never the server.
ed25519-compare: $(BUILD)/ed25519-compare
	@$(BUILD)/ed25519-compare

$(BUILD)/ed25519-compare: tools/ed25519_compare.cpp src/util/ed25519.cpp src/util/ed25519.h src/util/sha512.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) $(WARN) -o $@ $(filter %.cpp,$^) $(LDFLAGS) -lcrypto $(LDLIBS)

# How the stream budget copes with stalls and slow links; see tools/link_model.cpp.
link-model: $(BUILD)/link-model
	@$(BUILD)/link-model

$(BUILD)/link-model: tools/link_model.cpp src/core/stream_budget.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) $(WARN) -o $@ tools/link_model.cpp $(LDFLAGS) $(LDLIBS)

# A stand-in for a direct-sampling module such as Fern-RX888, for
# tools/wideband-check.sh; see tools/wideband_module.cpp.
wideband-module: $(BUILD)/wideband-module

$(BUILD)/wideband-module: tools/wideband_module.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) $(WARN) -o $@ tools/wideband_module.cpp $(LDFLAGS) $(LDLIBS)

# How each gain control profile treats signals whose right answer is known;
# see tools/agc_lab.cpp.
agc-lab: $(BUILD)/agc-lab
	@$(BUILD)/agc-lab

$(BUILD)/agc-lab: $(BUILD)/tools/agc_lab.o $(LIB_SRCS:%.cpp=$(BUILD)/%.o)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

bench: $(BUILD)/benchmark
	@$(BUILD)/benchmark

archive-bench: $(BUILD)/archive-bench
	@$(BUILD)/archive-bench

$(BUILD)/archive-bench: tools/archive_bench.cpp src/core/archive.cpp src/core/archive.h src/util/log.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) $(WARN) -o $@ tools/archive_bench.cpp src/core/archive.cpp $(LDFLAGS) $(LDLIBS)

$(BUILD)/benchmark: $(BUILD)/tools/benchmark.o $(LIB_SRCS:%.cpp=$(BUILD)/%.o)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# FFT timing and accuracy (tools/fft_bench.cpp). fft-compare builds the same
# program beside FFTW 3 in single precision; FFTW_PREFIX names a build outside
# the system paths. FFTW is linked into that tool only, never the server.
FFT_BENCH_OBJS := $(BUILD)/src/dsp/fft_split.o $(BUILD)/src/dsp/mdct.o
FFTW_PREFIX ?=
FFTW_FLAGS := $(if $(FFTW_PREFIX),-I$(FFTW_PREFIX)/include -L$(FFTW_PREFIX)/lib)

fft-bench: $(BUILD)/fft-bench
	@$(BUILD)/fft-bench

$(BUILD)/fft-bench: $(BUILD)/tools/fft_bench.o $(FFT_BENCH_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

fft-compare: $(BUILD)/fft-compare
	@FERNSDR_FFTW_WISDOM=$(BUILD)/fftw-wisdom.dat $(BUILD)/fft-compare

$(BUILD)/fft-compare: tools/fft_bench.cpp $(FFT_BENCH_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(OPT) $(WARN) -pthread -DFERNSDR_WITH_FFTW $(FFTW_FLAGS) -o $@ $^ $(LDFLAGS) -lfftw3f $(LDLIBS)

run: $(SERVER_BIN)
	$(SERVER_BIN) fernsdr.conf

clean:
	rm -rf $(BUILD) build-release-x86_64 build-release-aarch64 build-release-armhf dist

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
