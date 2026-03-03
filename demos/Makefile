# CSP Demos — standalone build against dist/ files
#
# Usage: make        (build all demos)
#        make run    (build + run all demos)
#        make clean  (remove build artifacts)
#
# Prerequisites: ../dist/csp.h, ../dist/csp.cpp, ../dist/csp_globals.cpp
# Run `make dist` in the parent directory to generate them.

CXX      := c++ -std=c++20 -stdlib=libc++
CXXFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter
BUILDDIR := build

DIST     := ../dist
CSP_OBJS := $(BUILDDIR)/csp.o $(BUILDDIR)/csp_globals.o

SRCS := $(wildcard *.cpp)
BINS := $(patsubst %.cpp,$(BUILDDIR)/%,$(SRCS))

.PHONY: all run clean

all: $(BINS)

run: $(BINS)
	@for bin in $(sort $(BINS)); do \
		echo "=== $$(basename $$bin) ==="; \
		./$$bin 2>/dev/null; \
		echo; \
	done

$(BUILDDIR)/%: %.cpp $(CSP_OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -I$(DIST) -o $@ $< $(CSP_OBJS)

$(BUILDDIR)/csp.o: $(DIST)/csp.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILDDIR)/csp_globals.o: $(DIST)/csp_globals.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)
