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

MAKEFLAGS += -j$(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Comma helper for $(subst) in BUILDDIR.
, := ,

BUILDDIR := build
CXX      := c++ -std=c++17 -stdlib=libc++
CXXFLAGS := -O2 -g -DDEBUG -Wall -Wextra -Wno-unused-parameter
LDFLAGS  :=
LDLIBS   :=

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
BUILDDIR := build-$(subst $(,),-,$(SANITIZE))
endif

ifneq ($(CSP_INCLUDE),include)
BUILDDIR := $(BUILDDIR)-$(CSP_INCLUDE)
endif

# --- Auto-dependencies ---
# -MMD generates .d files alongside .o files listing header deps.
# -MP adds phony targets for each header, preventing errors when
# headers are deleted/renamed.

DEPFLAGS = -MMD -MP

INCLUDES := -I$(CSP_INCLUDE) \
            -Ivendor/include

# --- TLS support (via mbedTLS) ---

CSP_TLS ?= 1

ifeq ($(CSP_TLS),1)
CXXFLAGS += -DCSP_TLS
INCLUDES += -Ivendor/github.com/Mbed-TLS/mbedtls/include
MBEDTLS_DIR    := vendor/github.com/Mbed-TLS/mbedtls/library
MBEDTLS_SRCS   := $(wildcard $(MBEDTLS_DIR)/*.c)
MBEDTLS_OBJS   := $(patsubst $(MBEDTLS_DIR)/%.c,$(BUILDDIR)/mbedtls/%.o,$(MBEDTLS_SRCS))
MBEDTLS_CFLAGS := -O2 -DMBEDTLS_CONFIG_FILE='"mbedtls_config.h"' \
                  -I$(CSP_INCLUDE) -Ivendor/github.com/Mbed-TLS/mbedtls/include
ifneq ($(SANITIZE),)
MBEDTLS_CFLAGS += -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
endif
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
            src/cancel.cc \
            src/channel.cc \
            src/clock.cc \
            src/hamt.cc \
            src/io.cc \
            src/log.cc \
            src/runtime.cpp \
            src/stack_analysis_arm64.cc \
            src/timer.cc \
            src/reactor.cc \
            src/blocking_pool.cc \
            src/signal.cc \
            src/stack_pool.cc
ifeq ($(CSP_TLS),1)
LIB_SRCS += src/tls.cc
endif
endif

TEST_SRCS    := test/main.cc $(wildcard test/*.test.cc)
BENCH_SRCS   := $(wildcard bench/*.bench.cc)
EXAMPLE_SRCS := $(wildcard examples/*.cc)

# --- Objects ---

LIB_OBJS   := $(patsubst %.cc,$(BUILDDIR)/%.o,$(patsubst %.cpp,$(BUILDDIR)/%.o,$(LIB_SRCS)))
ifneq ($(CSP_INCLUDE),dist)
LIB_OBJS   += $(FCONTEXT_OBJS)
endif
ifeq ($(CSP_TLS),1)
LIB_OBJS   += $(MBEDTLS_OBJS)
endif
TEST_OBJS  := $(patsubst %.cc,$(BUILDDIR)/%.o,$(TEST_SRCS))
BENCH_OBJS := $(patsubst %.cc,$(BUILDDIR)/%.o,$(BENCH_SRCS))

EXAMPLE_BINS := $(patsubst examples/%.cc,$(BUILDDIR)/examples/%,$(EXAMPLE_SRCS))

ALL_OBJS := $(LIB_OBJS) $(TEST_OBJS) $(BENCH_OBJS)
ALL_DEPS := $(ALL_OBJS:.o=.d)
TARGET       := $(BUILDDIR)/csp_tests
BENCH_TARGET := $(BUILDDIR)/csp_bench

# --- Rules ---

.PHONY: test build bench test-dist check check-tla-tags check-md-links diagrams examples run-examples dist iwyu clean

test: $(TARGET) check-md-links
	./$(TARGET)

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

# mbedTLS C sources
ifeq ($(CSP_TLS),1)
$(BUILDDIR)/mbedtls/%.o: $(MBEDTLS_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -c $(MBEDTLS_CFLAGS) -o $@ $<
endif

# Distribution sources (self-contained, no -Iinclude needed)
$(BUILDDIR)/dist/%.o: dist/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

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
	$(MAKE) CSP_INCLUDE=dist CSP_TLS=0 test

# --- include cleaner (clang-tidy) ---

TIDY_SYSROOT :=
ifneq ($(shell xcrun --show-sdk-path 2>/dev/null),)
TIDY_SYSROOT := -isysroot $(shell xcrun --show-sdk-path)
endif

TIDY_SRCS := $(filter-out src/stack_analysis_arm64.cc,$(LIB_SRCS))

iwyu: dist
	@python3 scripts/clean_includes.py $(TIDY_SRCS) \
		-- -std=c++17 -stdlib=libc++ $(TIDY_SYSROOT) $(INCLUDES)

clean:
	rm -rf build build-* dist

# Pull in generated dependency files (silently ignored on first build).
-include $(ALL_DEPS)
