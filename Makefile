# CSP — C++ microthreading with typed channels
# Usage: make                              (build + run tests)
#        make build                        (compile only)
#        make bench                        (build + run benchmarks)
#        make check                       (run TLA+ model checker)
#        make amalg                       (generate amalgamation files)
#        make iwyu                        (remove unused includes)
#        make clean                        (remove artifacts)
#        make SANITIZE=address,undefined   (ASan + UBSan)
#        make SANITIZE=thread              (TSan)
#        make examples                    (build examples)
#        make run-examples                (build + run examples)

# Comma helper for $(subst) in BUILDDIR.
, := ,

BUILDDIR := build
CXX      := c++ -std=c++17 -stdlib=libc++
CXXFLAGS := -O2 -g -DDEBUG -Wall -Wextra -Wno-unused-parameter
LDFLAGS  :=
LDLIBS   :=

# --- Sanitizer support ---
# Each sanitizer mode gets its own build directory so you can switch
# without cleaning.  ASan + UBSan and TSan are mutually exclusive.

ifneq ($(SANITIZE),)
CXXFLAGS += -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
LDFLAGS  += -fsanitize=$(SANITIZE)
BUILDDIR := build-$(subst $(,),-,$(SANITIZE))
endif

# --- Auto-dependencies ---
# -MMD generates .d files alongside .o files listing header deps.
# -MP adds phony targets for each header, preventing errors when
# headers are deleted/renamed.

DEPFLAGS = -MMD -MP

INCLUDES := -Iinclude \
            -Ithird_party

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

FCONTEXT_DIR    := third_party/boost-context/src/asm
FCONTEXT_SUFFIX := $(FCONTEXT_ARCH)_$(FCONTEXT_ABI)_$(FCONTEXT_FMT)_gas.S
FCONTEXT_SRCS   := $(FCONTEXT_DIR)/jump_$(FCONTEXT_SUFFIX) \
                   $(FCONTEXT_DIR)/make_$(FCONTEXT_SUFFIX)
FCONTEXT_OBJS   := $(patsubst $(FCONTEXT_DIR)/%.S,$(BUILDDIR)/fcontext/%.o,$(FCONTEXT_SRCS))

# --- Sources ---

LIB_SRCS := src/csp.cc \
            src/csp_globals.cpp \
            src/channel.cc \
            src/hamt.cc \
            src/mt_log.cc \
            src/runtime.cpp \
            src/stack_analysis_arm64.cc \
            src/reactor.cc \
            src/blocking_pool.cc \
            src/signal.cc \
            src/stack_pool.cc

TEST_SRCS    := test/main.cc $(wildcard test/*.test.cc)
BENCH_SRCS   := $(wildcard bench/*.bench.cc)
EXAMPLE_SRCS := $(wildcard examples/*.cc)

# --- Objects ---

LIB_OBJS   := $(patsubst %.cc,$(BUILDDIR)/%.o,$(patsubst %.cpp,$(BUILDDIR)/%.o,$(LIB_SRCS)))
LIB_OBJS   += $(FCONTEXT_OBJS)
TEST_OBJS  := $(patsubst %.cc,$(BUILDDIR)/%.o,$(TEST_SRCS))
BENCH_OBJS := $(patsubst %.cc,$(BUILDDIR)/%.o,$(BENCH_SRCS))

EXAMPLE_BINS := $(patsubst examples/%.cc,$(BUILDDIR)/examples/%,$(EXAMPLE_SRCS))

ALL_OBJS := $(LIB_OBJS) $(TEST_OBJS) $(BENCH_OBJS)
ALL_DEPS := $(ALL_OBJS:.o=.d)
TARGET       := $(BUILDDIR)/csp_tests
BENCH_TARGET := $(BUILDDIR)/csp_bench

# --- Rules ---

.PHONY: test build bench check examples run-examples amalg iwyu clean

test: $(TARGET)
	./$(TARGET)

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

check: $(TLA_JAR)
	@fail=0; \
	for spec in $(TLA_CHECK); do \
		echo "=== TLC: $$spec ==="; \
		java -XX:+UseParallelGC -jar $(TLA_JAR) -workers auto $$spec || fail=1; \
		echo; \
	done; \
	exit $$fail

amalg:
	python3 scripts/amalgamate.py

# --- include cleaner (clang-tidy) ---

TIDY_SYSROOT :=
ifneq ($(shell xcrun --show-sdk-path 2>/dev/null),)
TIDY_SYSROOT := -isysroot $(shell xcrun --show-sdk-path)
endif

TIDY_SRCS := $(filter-out src/stack_analysis_arm64.cc,$(LIB_SRCS))

iwyu: amalg
	@python3 scripts/clean_includes.py $(TIDY_SRCS) amalg/csp.cpp \
		-- -std=c++17 -stdlib=libc++ $(TIDY_SYSROOT) $(INCLUDES)

clean:
	rm -rf build build-* amalg

# Pull in generated dependency files (silently ignored on first build).
-include $(ALL_DEPS)
