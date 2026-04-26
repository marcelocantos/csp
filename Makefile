# CSP — C++ imp-based concurrency with typed channels
# Usage: make                              (build + run tests)
#        make build                        (compile only)
#        make bench                        (build + run benchmarks)
#        make test-dist                    (test against distribution build)
#        make check                       (run TLA+ model checker)
#        make dist                        (generate distribution files)
#        make iwyu                        (remove unused includes)
#        make clean                        (remove artifacts)
#        make SANITIZE=address,undefined   (ASan + UBSan)
#        make SANITIZE=thread              (TSan)
#        make examples                    (build examples)
#        make run-examples                (build + run examples)
#        make docker-test                 (Linux ARM64+x86 in Docker)
#        make docker-test-arm64           (Linux ARM64 in Docker)
#        make docker-test-x86             (Linux x86_64 in Docker)
#        make docker-test-arm64 SANITIZE=thread (sanitizers in Docker)

MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Comma helper for $(subst) in BUILDDIR.
, := ,

BUILDDIR := build/normal
CXX      := c++ -std=c++20 -stdlib=libc++
CXXFLAGS := -O2 -g -DDEBUG -Wall -Wextra -Wno-unused-parameter
LDFLAGS  :=
LDLIBS   :=

# --- ccache (compiler cache) ---
# Auto-detected if installed. Set CCACHE=no to disable. Caches object
# files by preprocessed-source hash so repeated clean builds and
# branch-switches that touch unchanged files are near-instant.
CCACHE ?= $(shell command -v ccache 2>/dev/null)
ifneq ($(CCACHE),)
ifneq ($(CCACHE),no)
CXX := $(CCACHE) $(CXX)
CC  := $(CCACHE) $(CC)
endif
endif

# --- Include path ---
# CSP_INCLUDE selects header source: 'include' for development,
# 'dist' for distribution.  test-dist uses recursive make to switch.

CSP_INCLUDE ?= include

# --- Sanitizer support ---
# Each sanitizer mode gets its own build directory so you can switch
# without cleaning.  ASan + UBSan and TSan are mutually exclusive.

ifneq ($(SANITIZE),)
CXXFLAGS += -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
LDFLAGS  += -fsanitize=$(SANITIZE)
BUILDDIR := build/$(subst $(,),-,$(SANITIZE))
endif

ifneq ($(CSP_INCLUDE),include)
BUILDDIR := $(BUILDDIR)-$(CSP_INCLUDE)
endif

ifeq ($(CSP_TLS),0)
BUILDDIR := $(BUILDDIR)-notls
endif

# --- Auto-dependencies ---
# -MMD generates .d files alongside .o files listing header deps.
# -MP adds phony targets for each header, preventing errors when
# headers are deleted/renamed.

DEPFLAGS = -MMD -MP

NGHTTP2_DIR := vendor/github.com/nghttp2/nghttp2/lib
NGHTTP2_INC := $(NGHTTP2_DIR)/includes

INCLUDES := -I$(CSP_INCLUDE) \
            -Ivendor/include \
            -Ivendor/github.com/nodejs/llhttp/include \
            -I$(NGHTTP2_INC)

# --- TLS support (via PicoTLS + minicrypto) ---

CSP_TLS ?= 1

ifeq ($(CSP_TLS),1)
CXXFLAGS += -DCSP_TLS

PICOTLS_DIR  := vendor/github.com/h2o/picotls
CIFRA_DIR    := $(PICOTLS_DIR)/deps/cifra/src
UECC_DIR     := $(PICOTLS_DIR)/deps/micro-ecc

INCLUDES += -I$(PICOTLS_DIR)/include \
            -I$(CIFRA_DIR) \
            -I$(CIFRA_DIR)/ext \
            -I$(UECC_DIR)


PICOTLS_SRCS := $(PICOTLS_DIR)/lib/picotls.c \
                $(PICOTLS_DIR)/lib/hpke.c \
                $(PICOTLS_DIR)/lib/pembase64.c \
                $(PICOTLS_DIR)/lib/cifra.c \
                $(PICOTLS_DIR)/lib/cifra/x25519.c \
                $(PICOTLS_DIR)/lib/cifra/chacha20.c \
                $(PICOTLS_DIR)/lib/cifra/aes128.c \
                $(PICOTLS_DIR)/lib/cifra/aes256.c \
                $(PICOTLS_DIR)/lib/cifra/random.c \
                $(PICOTLS_DIR)/lib/uecc.c \
                $(PICOTLS_DIR)/lib/minicrypto-pem.c \
                $(PICOTLS_DIR)/lib/asn1.c \
                $(PICOTLS_DIR)/lib/ffx.c \
                $(UECC_DIR)/uECC.c \
                $(CIFRA_DIR)/aes.c \
                $(CIFRA_DIR)/blockwise.c \
                $(CIFRA_DIR)/chacha20.c \
                $(CIFRA_DIR)/chash.c \
                $(CIFRA_DIR)/curve25519.c \
                $(CIFRA_DIR)/drbg.c \
                $(CIFRA_DIR)/hmac.c \
                $(CIFRA_DIR)/gcm.c \
                $(CIFRA_DIR)/gf128.c \
                $(CIFRA_DIR)/modes.c \
                $(CIFRA_DIR)/poly1305.c \
                $(CIFRA_DIR)/sha256.c \
                $(CIFRA_DIR)/sha512.c

PICOTLS_OBJS := $(patsubst %.c,$(BUILDDIR)/%.o,$(PICOTLS_SRCS))

PICOTLS_CFLAGS := -O2 -I$(PICOTLS_DIR)/include \
                  -I$(CIFRA_DIR) -I$(CIFRA_DIR)/ext -I$(UECC_DIR)
ifneq ($(SANITIZE),)
PICOTLS_CFLAGS += -fsanitize=$(SANITIZE) -fno-omit-frame-pointer \
                  -fno-sanitize=pointer-overflow
endif

endif

# --- llhttp (vendored HTTP parser) ---

LLHTTP_DIR  := vendor/github.com/nodejs/llhttp
LLHTTP_SRCS := $(LLHTTP_DIR)/src/llhttp.c \
               $(LLHTTP_DIR)/src/api.c \
               $(LLHTTP_DIR)/src/http.c
LLHTTP_OBJS := $(patsubst %.c,$(BUILDDIR)/%.o,$(LLHTTP_SRCS))

LLHTTP_CFLAGS := -O2 -I$(LLHTTP_DIR)/include
ifneq ($(SANITIZE),)
LLHTTP_CFLAGS += -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
endif

# --- nghttp2 (vendored HTTP/2 session management) ---

NGHTTP2_SRCS := $(NGHTTP2_DIR)/nghttp2_alpn.c \
                $(NGHTTP2_DIR)/nghttp2_buf.c \
                $(NGHTTP2_DIR)/nghttp2_callbacks.c \
                $(NGHTTP2_DIR)/nghttp2_debug.c \
                $(NGHTTP2_DIR)/nghttp2_extpri.c \
                $(NGHTTP2_DIR)/nghttp2_frame.c \
                $(NGHTTP2_DIR)/nghttp2_hd.c \
                $(NGHTTP2_DIR)/nghttp2_hd_huffman.c \
                $(NGHTTP2_DIR)/nghttp2_hd_huffman_data.c \
                $(NGHTTP2_DIR)/nghttp2_helper.c \
                $(NGHTTP2_DIR)/nghttp2_http.c \
                $(NGHTTP2_DIR)/nghttp2_map.c \
                $(NGHTTP2_DIR)/nghttp2_mem.c \
                $(NGHTTP2_DIR)/nghttp2_option.c \
                $(NGHTTP2_DIR)/nghttp2_outbound_item.c \
                $(NGHTTP2_DIR)/nghttp2_pq.c \
                $(NGHTTP2_DIR)/nghttp2_priority_spec.c \
                $(NGHTTP2_DIR)/nghttp2_queue.c \
                $(NGHTTP2_DIR)/nghttp2_ratelim.c \
                $(NGHTTP2_DIR)/nghttp2_rcbuf.c \
                $(NGHTTP2_DIR)/nghttp2_session.c \
                $(NGHTTP2_DIR)/nghttp2_stream.c \
                $(NGHTTP2_DIR)/nghttp2_submit.c \
                $(NGHTTP2_DIR)/nghttp2_time.c \
                $(NGHTTP2_DIR)/nghttp2_version.c \
                $(NGHTTP2_DIR)/sfparse.c
NGHTTP2_OBJS := $(patsubst %.c,$(BUILDDIR)/%.o,$(NGHTTP2_SRCS))

# nghttp2ver.h is generated from the .in template by autotools/cmake; we
# substitute the version constants directly so a bare submodule clone builds
# without running their build system.
NGHTTP2_VERSION_STR := 1.69.90
NGHTTP2_VERSION_NUM := 0x01455a
NGHTTP2_VER_H       := $(NGHTTP2_INC)/nghttp2/nghttp2ver.h

$(NGHTTP2_VER_H): $(NGHTTP2_INC)/nghttp2/nghttp2ver.h.in
	sed -e 's/@PACKAGE_VERSION@/$(NGHTTP2_VERSION_STR)/' \
	    -e 's/@PACKAGE_VERSION_NUM@/$(NGHTTP2_VERSION_NUM)/' \
	    $< > $@

$(NGHTTP2_OBJS): $(NGHTTP2_VER_H)
$(BUILDDIR)/src/http2.o: $(NGHTTP2_VER_H)

NGHTTP2_CFLAGS := -O2 -I$(NGHTTP2_INC) -DBUILDING_NGHTTP2
ifneq ($(SANITIZE),)
NGHTTP2_CFLAGS += -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
endif

# --- fcontext (vendored from Boost.Context) ---

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
  FCONTEXT_FMT := macho
else
  FCONTEXT_FMT := elf
endif

ifneq (,$(filter arm64 aarch64,$(UNAME_M)))
  FCONTEXT_ARCH := arm64
  FCONTEXT_ABI  := aapcs
else
  FCONTEXT_ARCH := x86_64
  FCONTEXT_ABI  := sysv
endif

FCONTEXT_DIR    := vendor/github.com/boostorg/context/src/asm
FCONTEXT_SUFFIX := $(FCONTEXT_ARCH)_$(FCONTEXT_ABI)_$(FCONTEXT_FMT)_gas.S
FCONTEXT_SRCS   := $(FCONTEXT_DIR)/jump_$(FCONTEXT_SUFFIX) \
                   $(FCONTEXT_DIR)/make_$(FCONTEXT_SUFFIX)
FCONTEXT_OBJS   := $(patsubst $(FCONTEXT_DIR)/%.S,$(BUILDDIR)/fcontext/%.o,$(FCONTEXT_SRCS))

# --- Sources ---

ifeq ($(CSP_INCLUDE),dist)
LIB_SRCS := dist/csp.cpp \
            dist/csp_globals.cpp
else
LIB_SRCS := src/csp.cc \
            src/csp_globals.cpp \
            src/byte_reader.cc \
            src/cancel.cc \
            src/channel.cc \
            src/clock.cc \
            src/hamt.cc \
            src/io.cc \
            src/log.cc \
            src/runtime.cpp \
            src/stack_analysis_arm64.cc \
            src/supervisor.cc \
            src/timer.cc \
            src/reactor.cc \
            src/blocking_pool.cc \
            src/signal.cc \
            src/stack_pool.cc \
            src/imp_exit.cc \
            src/net.cc \
            src/file.cc
LIB_SRCS += src/http.cc
LIB_SRCS += src/http2.cc
ifeq ($(CSP_TLS),1)
LIB_SRCS += src/tls.cc
endif
endif

TEST_SRCS    := test/main.cc $(wildcard test/*.test.cc)
# net.test.cc and http.test.cc depend on headers not in the dist amalgamation.
ifeq ($(CSP_INCLUDE),dist)
TEST_SRCS    := $(filter-out test/net.test.cc test/http.test.cc,$(TEST_SRCS))
endif
BENCH_SRCS   := $(wildcard bench/*.bench.cc)
EXAMPLE_SRCS := $(wildcard examples/*.cc)

# --- Objects ---

LIB_OBJS   := $(patsubst %.cc,$(BUILDDIR)/%.o,$(patsubst %.cpp,$(BUILDDIR)/%.o,$(LIB_SRCS)))
ifneq ($(CSP_INCLUDE),dist)
LIB_OBJS   += $(FCONTEXT_OBJS) $(LLHTTP_OBJS) $(NGHTTP2_OBJS)
endif
ifeq ($(CSP_TLS),1)
LIB_OBJS   += $(PICOTLS_OBJS)
endif
TEST_OBJS  := $(patsubst %.cc,$(BUILDDIR)/%.o,$(TEST_SRCS))
BENCH_OBJS := $(patsubst %.cc,$(BUILDDIR)/%.o,$(BENCH_SRCS))

EXAMPLE_BINS := $(patsubst examples/%.cc,$(BUILDDIR)/examples/%,$(EXAMPLE_SRCS))

ALL_OBJS := $(LIB_OBJS) $(TEST_OBJS) $(BENCH_OBJS)
ALL_DEPS := $(ALL_OBJS:.o=.d)
TARGET       := $(BUILDDIR)/csp_tests
BENCH_TARGET := $(BUILDDIR)/csp_bench

# --- Rules ---

.PHONY: test build bench test-dist check check-tla-tags check-md-links diagrams examples run-examples dist iwyu clean \
       docker-test docker-test-arm64 docker-test-x86 docker-image docker-clean bullseye

# Explicit default — keep `make` (no args) running the full test suite.
# Without this, the `bullseye` rule below would become the default target
# by virtue of appearing first.
.DEFAULT_GOAL := test

test: $(TARGET) check-md-links
	./$(TARGET)

# --- Standing invariants (consumed by `bullseye_convergence`) ---
# Exit 0 if all green, non-zero on first violation. Stdout is relayed
# verbatim to the agent. Tests are bounded with a portable timeout so a
# flake doesn't wedge convergence checks. macOS doesn't ship GNU
# `timeout`, so we prefer `gtimeout` (coreutils) and fall back to a
# perl alarm wrapper available on every Unix.
BULLSEYE_TEST_TIMEOUT ?= 120
BULLSEYE_TIMEOUT_CMD := $(shell command -v timeout 2>/dev/null \
                            || command -v gtimeout 2>/dev/null \
                            || echo "perl -e 'alarm shift; exec @ARGV' --")
bullseye:
	@$(MAKE) --no-print-directory build && echo "✓ build"
	@$(BULLSEYE_TIMEOUT_CMD) $(BULLSEYE_TEST_TIMEOUT) ./$(TARGET) --no-colors --reporters=console >/dev/null && echo "✓ tests" || \
	 (echo "✗ tests (exit $$? — timeout=$(BULLSEYE_TEST_TIMEOUT)s)"; exit 1)
	@python3 scripts/check_md_links.py >/dev/null && echo "✓ md-links" || \
	 (echo "✗ md-links"; python3 scripts/check_md_links.py; exit 1)
	@test -z "$$(git status --porcelain)" && echo "✓ clean tree" || \
	 (echo "✗ dirty tree"; git status --short; exit 1)

check-md-links: diagrams
	@python3 scripts/check_md_links.py

# --- Diagram generation ---
# Scans docs/**/*.md for <!-- csp-flow ... --> blocks and emits SVGs.

diagrams:
	@python3 scripts/gen_diagrams.py

build: $(TARGET)

bench: $(BENCH_TARGET)
	./$(BENCH_TARGET)

$(TARGET): $(LIB_OBJS) $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BENCH_TARGET): $(LIB_OBJS) $(BENCH_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Library sources
$(BUILDDIR)/src/%.o: src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c -o $@ $<

# Vendored fcontext assembly
$(BUILDDIR)/fcontext/%.o: $(FCONTEXT_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CXX) -c -o $@ $<

# llhttp C sources
$(BUILDDIR)/$(LLHTTP_DIR)/%.o: $(LLHTTP_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(LLHTTP_CFLAGS) -o $@ $<

# nghttp2 C sources
$(BUILDDIR)/$(NGHTTP2_DIR)/%.o: $(NGHTTP2_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(NGHTTP2_CFLAGS) -o $@ $<

# PicoTLS + minicrypto C sources
ifeq ($(CSP_TLS),1)
$(BUILDDIR)/$(PICOTLS_DIR)/%.o: $(PICOTLS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(PICOTLS_CFLAGS) -o $@ $<

$(BUILDDIR)/$(CIFRA_DIR)/%.o: $(CIFRA_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(PICOTLS_CFLAGS) -o $@ $<

$(BUILDDIR)/$(UECC_DIR)/%.o: $(UECC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(PICOTLS_CFLAGS) -o $@ $<

# ngtcp2 C sources (lib + crypto adapter)
$(BUILDDIR)/$(NGTCP2_DIR)/%.o: $(NGTCP2_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(NGTCP2_CFLAGS) -o $@ $<

endif

# Distribution sources
$(BUILDDIR)/dist/%.o: dist/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c -o $@ $<

# Test sources
$(BUILDDIR)/test/%.o: test/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -Itest -c -o $@ $<

# Benchmark sources
$(BUILDDIR)/bench/%.o: bench/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c -o $@ $<

# --- Examples ---

examples: $(EXAMPLE_BINS)

run-examples: $(EXAMPLE_BINS)
	@for bin in $(EXAMPLE_BINS); do \
		echo "=== $$(basename $$bin) ==="; \
		./$$bin; \
		echo; \
	done

$(BUILDDIR)/examples/%: examples/%.cc $(LIB_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ $< $(LIB_OBJS) $(LDLIBS)

# --- TLA+ model checking ---

TLA_JAR    := formal/tla2tools.jar
TLA_SPECS  := $(wildcard formal/*.tla)
# Exclude TLC-generated trace-exploration specs.
TLA_SPECS  := $(foreach s,$(TLA_SPECS),$(if $(findstring _TTrace,$(s)),,$(s)))
# Exclude _Bug specs from the default check (they demonstrate known violations).
TLA_CHECK  := $(filter-out %_Bug.tla,$(TLA_SPECS))

check-tla-tags:
	@python3 scripts/check_tla_tags.py

check: $(TLA_JAR)
	@fail=0; \
	for spec in $(TLA_CHECK); do \
		echo "=== TLC: $$spec ==="; \
		java -XX:+UseParallelGC -jar $(TLA_JAR) -workers auto $$spec || fail=1; \
		echo; \
	done; \
	exit $$fail

dist:
	python3 scripts/amalgamate.py

test-dist: dist
ifneq ($(findstring thread,$(SANITIZE)),)
	$(MAKE) CSP_INCLUDE=dist CSP_TLS=0 test
else
	$(MAKE) CSP_INCLUDE=dist CSP_TLS=1 test
	$(MAKE) CSP_INCLUDE=dist CSP_TLS=0 test
endif

# --- include cleaner (clang-tidy) ---

TIDY_SYSROOT :=
ifneq ($(shell xcrun --show-sdk-path 2>/dev/null),)
TIDY_SYSROOT := -isysroot $(shell xcrun --show-sdk-path)
endif

TIDY_SRCS := $(filter-out src/stack_analysis_arm64.cc,$(LIB_SRCS))

iwyu: dist
	@python3 scripts/clean_includes.py $(TIDY_SRCS) \
		-- -std=c++20 -stdlib=libc++ $(TIDY_SYSROOT) $(INCLUDES)

# --- Docker Linux testing ---
# Build image once per platform; reuse on subsequent runs.

DOCKER_PLATFORM ?= linux/arm64
DOCKER_TAG       = csp-test-$(subst /,-,$(DOCKER_PLATFORM))
DOCKER_CXX      := clang++-18 -std=c++20 -stdlib=libc++
DOCKER_BUILDDIR  = build/$(subst /,-,$(DOCKER_PLATFORM))

docker-image:
	printf '%s\n' \
		'FROM ubuntu:24.04' \
		'RUN apt-get update && apt-get install -y --no-install-recommends clang-18 libc++-18-dev libc++abi-18-dev make python3 git' \
	| docker build --platform $(DOCKER_PLATFORM) -t $(DOCKER_TAG) -

docker-run-test:
	@docker image inspect $(DOCKER_TAG) >/dev/null 2>&1 || $(MAKE) docker-image DOCKER_PLATFORM=$(DOCKER_PLATFORM)
	docker run --rm --platform $(DOCKER_PLATFORM) \
		-v "$(CURDIR):/src" -w /src $(DOCKER_TAG) \
		make BUILDDIR="$(DOCKER_BUILDDIR)" CC=clang-18 CXX="$(DOCKER_CXX)" $(if $(SANITIZE),SANITIZE=$(SANITIZE),)

docker-test: docker-test-arm64 docker-test-x86

docker-test-arm64:
	$(MAKE) docker-run-test DOCKER_PLATFORM=linux/arm64

docker-test-x86:
	$(MAKE) docker-run-test DOCKER_PLATFORM=linux/amd64

docker-clean:
	-docker rmi csp-test-linux-arm64 csp-test-linux-amd64 2>/dev/null

clean:
	rm -rf build

# Pull in generated dependency files (silently ignored on first build).
-include $(ALL_DEPS)
