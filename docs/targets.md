# Convergence Targets

<!-- last-evaluated: 1353a4a -->

## Active

### 🎯T1 Windows port PR is merged to master  [high]

CSP builds, links, and passes all tests on Windows x86_64 via MSVC/CMake,
and the PR (#4) is merged to master with all CI green.

- **Value**: 8 — unlocks Windows as a supported platform, major adoption gate
- **Cost**: 5 — mostly debugging; architectural work done
- **Priority**: high
- **Status**: converging (CI partially working, 3 distinct failure classes remain)
- **Acceptance criteria**:
  - All 11 CI jobs pass (macOS test, Linux test x2, sanitizers x6, Windows, TLA+)
  - `make dist` output matches committed dist/ files
  - Windows test exe runs doctest and reports pass/fail (not silent crash)
  - PR squash-merged to master

#### 🎯T1.1 Dist files are regenerated and committed  [high]

The `dist/` files in the branch reflect the current source, including all
new headers (count.h, win/signal.h, etc.) and platform guards.

- **Value**: 8 (gates 10/11 CI jobs)
- **Cost**: 1 — just run `make dist` and commit
- **Priority**: high (highest leverage — fixes 10 of 11 failing jobs)
- **Status**: identified
- **Parent**: 🎯T1
- **Acceptance criteria**:
  - `make dist && git diff --exit-code dist/` passes
  - Sanitizer and TSan jobs can compile test-dist

#### 🎯T1.2 macOS mn.test MultipleThreads flake is fixed  [medium]

The M:N threading test `CHECK(thread_ids.size() > 1)` passes reliably on
macOS CI runners, which may have limited parallelism.

- **Value**: 3
- **Cost**: 2 — likely needs a WARN or retry strategy
- **Priority**: medium
- **Status**: identified
- **Parent**: 🎯T1
- **Acceptance criteria**:
  - macOS arm64 test job passes reliably (no flake on thread count check)

#### 🎯T1.3 Windows test exe runs and reports results  [high]

The Windows build's `csp_tests.exe` currently crashes at startup (exit code 1,
no doctest output). It must run tests and report pass/fail.

- **Value**: 5
- **Cost**: 5 — likely fcontext init, reactor, or TLS globals issue
- **Priority**: high
- **Status**: identified
- **Parent**: 🎯T1
- **Acceptance criteria**:
  - `csp_tests.exe` prints doctest summary on Windows CI
  - Exit code reflects test results (0 = all pass)
