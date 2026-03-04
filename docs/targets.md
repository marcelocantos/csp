# Targets

<!-- last-evaluated: 246e063 -->

## Active

### 🎯T1 Windows port PR is merged to master
- **Weight**: 2 (value 8 / cost 5)
- **Estimated-cost**: 5
- **Acceptance**:
  - All 11 CI jobs pass (macOS test, Linux test x2, sanitizers x6, Windows, TLA+)
  - `make dist` output matches committed dist/ files
  - Windows test exe runs doctest and reports pass/fail (not silent crash)
  - PR #4 squash-merged to master
- **Context**: Unlocks Windows as a supported platform — major adoption gate. Architectural work (CMake, platform guards, VirtualAlloc stack pool, reactor, socket I/O) is done. Remaining work is debugging CI failures.
- **Status**: converging
- **Discovered**: 2026-03-01

### 🎯T1.2 macOS mn.test MultipleThreads flake is fixed
- **Weight**: 2 (value 3 / cost 2)
- **Estimated-cost**: 2
- **Acceptance**: macOS arm64 CI test job passes reliably (no flake on `CHECK(thread_ids.size() > 1)`)
- **Context**: The M:N threading test assumes CI runners schedule work across >1 OS thread, but low-core runners may not. Needs WARN or yield-based fix.
- **Parent**: 🎯T1
- **Status**: identified
- **Discovered**: 2026-03-04

### 🎯T1.3 Windows test exe runs and reports results
- **Weight**: 1 (value 5 / cost 5)
- **Estimated-cost**: 5
- **Acceptance**:
  - `csp_tests.exe` prints doctest summary on Windows CI
  - Exit code reflects test results (0 = all pass)
- **Context**: Windows build links successfully but test exe crashes at startup (exit code 1, no doctest output). Likely fcontext init, reactor, or TLS globals issue.
- **Parent**: 🎯T1
- **Status**: identified
- **Discovered**: 2026-03-04

## Achieved

### 🎯T1.1 Dist files are regenerated and committed
- **Weight**: 8 (value 8 / cost 1)
- **Estimated-cost**: 1
- **Actual-cost**: 1
- **Acceptance**:
  - `make dist && git diff --exit-code dist/` passes
  - Sanitizer and TSan jobs can compile test-dist
- **Context**: dist/ files were stale from master merge, missing count.h, win/signal.h, platform guards, and log→s_log renames. Gated 10/11 CI jobs.
- **Parent**: 🎯T1
- **Status**: achieved
- **Discovered**: 2026-03-04
- **Achieved**: 2026-03-04
