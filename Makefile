# CSP — C++ microthreading with typed channels
# Usage: make                              (build + run tests)
#        make build                        (compile only)
#        make clean                        (remove artifacts)
#        make SANITIZE=address,undefined   (ASan + UBSan)
#        make SANITIZE=thread              (TSan)

# Comma helper for $(subst) in BUILDDIR.
, := ,

BUILDDIR := build
CXX      := c++ -std=c++17 -stdlib=libc++
CXXFLAGS := -O2 -g -DDEBUG -Wall -Wextra -Wno-unused-parameter
LDFLAGS  := -L/opt/homebrew/lib
LDLIBS   := -lboost_context

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
            -Ithird_party \
            -I/opt/homebrew/include

# --- Sources ---

LIB_SRCS := src/microthread.cc \
            src/microthread_globals.cpp \
            src/channel.cc \
            src/mt_log.cc \
            src/runtime.cpp

TEST_SRCS := test/main.cc $(wildcard test/*.test.cc)

# --- Objects ---

LIB_OBJS   := $(patsubst %.cc,$(BUILDDIR)/%.o,$(patsubst %.cpp,$(BUILDDIR)/%.o,$(LIB_SRCS)))
TEST_OBJS  := $(patsubst %.cc,$(BUILDDIR)/%.o,$(TEST_SRCS))

ALL_OBJS := $(LIB_OBJS) $(TEST_OBJS)
ALL_DEPS := $(ALL_OBJS:.o=.d)
TARGET   := $(BUILDDIR)/csp_tests

# --- Rules ---

.PHONY: test build clean

test: $(TARGET)
	./$(TARGET)

build: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Library sources
$(BUILDDIR)/src/%.o: src/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -c -o $@ $<

# Test sources
$(BUILDDIR)/test/%.o: test/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) $(INCLUDES) -Itest -c -o $@ $<

clean:
	rm -rf build build-*

# Pull in generated dependency files (silently ignored on first build).
-include $(ALL_DEPS)
