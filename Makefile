# CSP — C++ microthreading with typed channels
# Usage: make          (build + run tests)
#        make build    (compile only)
#        make clean    (remove artifacts)

BUILDDIR := build

CXX      := c++ -std=c++17 -stdlib=libc++
CXXFLAGS := -O2 -g -DDEBUG -Wall -Wextra -Wno-unused-parameter

INCLUDES := -Iinclude \
            -Ithird_party \
            -I/opt/homebrew/include

# --- Sources ---

LIB_SRCS := src/microthread.cc \
            src/microthread_globals.cpp \
            src/channel.cc \
            src/ringbuffer.cc \
            src/mt_log.cc

TEST_SRCS := test/main.cc $(wildcard test/*.test.cc)

# --- Objects ---

LIB_OBJS   := $(patsubst %.cc,$(BUILDDIR)/%.o,$(patsubst %.cpp,$(BUILDDIR)/%.o,$(LIB_SRCS)))
TEST_OBJS  := $(patsubst %.cc,$(BUILDDIR)/%.o,$(TEST_SRCS))

ALL_OBJS := $(LIB_OBJS) $(TEST_OBJS)
TARGET   := $(BUILDDIR)/csp_tests

LDFLAGS  := -L/opt/homebrew/lib
LDLIBS   := -lboost_context

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
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

$(BUILDDIR)/src/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c -o $@ $<

# Test sources
$(BUILDDIR)/test/%.o: test/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Itest -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)
