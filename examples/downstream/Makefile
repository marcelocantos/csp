# Downstream link-test for pre-built CSP library artefacts (🎯T24).
#
# Simulates an external user who has ONLY the release tarballs:
#   - the include/ archive  (csp.h)
#   - the per-platform lib archive (libcsp, libcsp_http, libllhttp, ...)
# No CSP sources, no vendored third-party sources.
#
# Point CSP_INCLUDE at the unpacked headers and CSP_LIB at the unpacked
# libraries, then `make`. Defaults match scripts/build-libs.sh's output dir
# so a local build "just works":
#
#   make CSP_INCLUDE=/path/to/include CSP_LIB=/path/to/lib
#
# The link incantation here is the canonical one documented in the release
# notes: -L$(CSP_LIB) -lcsp_http -lcsp -lllhttp, plus the platform's
# dead-strip flag so unreferenced protocol code drops out.

REPO_ROOT   := ../..
CSP_INCLUDE ?= $(REPO_ROOT)/build/libs
CSP_LIB     ?= $(REPO_ROOT)/build/libs

CXX      ?= c++
CXXFLAGS := -std=c++20 -stdlib=libc++ -O2 \
            -ffunction-sections -fdata-sections \
            -I$(CSP_INCLUDE)

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  DEADSTRIP := -Wl,-dead_strip
  # Static-link the CSP + protocol libs; rpath lets the shared variant work too.
  RPATH     := -Wl,-rpath,$(abspath $(CSP_LIB))
else
  DEADSTRIP := -Wl,--gc-sections
  RPATH     := -Wl,-rpath,$(abspath $(CSP_LIB))
endif

# Link order: protocol lib before core before its vendored dep. The HTTP
# drop-in references core symbols; llhttp is referenced by the HTTP drop-in.
# (-lcsp_http pulls llhttp; both pull csp.) GNU ld is order-sensitive, so the
# dependency-last ordering below works on both ld64 and lld/GNU ld.
LDLIBS := -L$(CSP_LIB) -lcsp_http -lcsp -lllhttp

.PHONY: all run clean
all: downstream

downstream: downstream.cc
	$(CXX) $(CXXFLAGS) -o $@ downstream.cc $(LDLIBS) $(DEADSTRIP) $(RPATH)

run: downstream
	./downstream

clean:
	rm -f downstream
