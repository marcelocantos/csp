# Convergence Targets

## Active

### 🎯T2 Windows port
- **Priority**: medium
- **Weight**: 5 (value 5 / cost 8)
- **Status**: converging
- **Acceptance criteria**:
  - Windows CI job compiles, links, and runs all non-platform-specific tests
  - Platform-specific tests (signal, kqueue/epoll reactor, unix sockets) are `#ifndef _WIN32` guarded
  - `make test-dist` passes on Windows (or CMake equivalent)
- **Blocked by**: startup crash diagnosis (see PR #4)

## Achieved

### 🎯T3 Imp exit / supervision documentation
- **Priority**: medium
- **Weight**: 4 (value 4 / cost 2)
- **Status**: achieved
- **Achieved**: 2026-03-07
- **Acceptance criteria**:
  - `docs/reference/imp-exit.md` exists with standard reference page structure
  - `docs/agent-guide.md` Lifecycle section includes imp_exit/supervised entries
  - `max_restarts_exceeded` disposition: documented as available for custom handlers; built-in policy doesn't throw it
  - `make` passes (markdown link checker)

### 🎯T1 Test-dist exercises both TLS modes
- **Priority**: high
- **Weight**: 8 (value 8 / cost 1)
- **Status**: achieved
- **Achieved**: 2026-03-06 (PR #6, commit 86ba5e3)
- **Acceptance criteria**:
  - `make test-dist` runs tests with `CSP_TLS=1` (636 tests) then `CSP_TLS=0` (628 tests)
  - `MBEDTLS_CFLAGS` uses `-Iinclude` (not `-I$(CSP_INCLUDE)`) so `mbedtls_config.h` is found in dist mode
  - `BUILDDIR` differentiates TLS=0 from TLS=1 to prevent stale object reuse
  - `test/tls.test.cc` compiles against dist header (no `#include <csp/tls.h>` — uses gateway header via testutil.h)
  - All 18 CI jobs green after merge
