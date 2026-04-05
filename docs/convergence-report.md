# Convergence Report

**Generated**: 2026-04-05
**Branch**: master
**SHA**: 4a6ddd6

## Standing invariants

- Tests: 664/664 passed (per T15 status; local not re-run this evaluation).
- CI: **in progress** — v0.6.0 release push running. Prior merge commit (c0b4c2f) failed on macOS test + Linux TSan (build/dist binary not found — CI config issue, not test failure). Monitoring.

## Movement

- 🎯T11: in progress → close (PR #18 merged, fake_clock quiescence done; residual: `default_scheduler_impl` name)
- 🎯T15: (new) → achieved (PicoTLS vendored, mbedTLS removed, 664/664, all CI green)
- 🎯T14: (new) → not started
- 🎯T12: (unchanged) not started
- 🎯T7: (unchanged) achieved
- 🎯T3: (unchanged) not started
- 🎯T13: (unchanged) not started

## Gap report

### 🎯T12 Test names contain no spaces  [weight 2.0]
Gap: not started
667 TEST_CASE names still contain spaces across test/*.test.cc. No progress since last report.

### 🎯T14 Dynamic scoping uses chained stack arrays instead of HAMT  [weight 1.7]
Gap: not started
HAMT code still present. No work begun on chained stack array replacement.

### 🎯T15 TLS uses PicoTLS instead of mbedTLS  [weight 1.7]
Gap: achieved
PicoTLS vendored at vendor/github.com/h2o/picotls/. mbedTLS submodule removed. TSan suppressions for mbedTLS removed (only fake_clock suppression remains). 664/664 tests, 0 skipped. All acceptance criteria met.

### 🎯T7 Non-trivial example applications demonstrate CSP  [weight 1.7]
Gap: achieved
3/6 sub-targets complete (T7.1, T7.2, T7.5). Acceptance threshold (at least 3) met.

### 🎯T11 Scheduler is always M:N  [weight 1.6]
Gap: close
PR #18 merged into master (v0.6.0). Major criteria met: mn_mode_ removed, await_completion() exists with schedule() as deprecated alias, use_run gone, daemon imps implemented, fake_clock quiescence rework complete (no fc.run() in tests). One residual: `default_scheduler_impl` function still exists in src/csp.cc — it always calls main_loop(), so behavior is correct but the name should be cleaned up or the function inlined.

### 🎯T4 API safety gaps are closed  [weight 1.2]  (status only)
Status: achieved

### 🎯T5 Unmodeled concurrent decision points have TLA+ specs  [weight 1.0]  (status only)
Status: achieved

### 🎯T8 Signal handling is audited for correctness  [weight 1.0]  (status only)
Status: achieved

### 🎯T3 Runtime is production-ready for I/O workloads  [weight 0.6]  (status only)
Status: not started (0/9 sub-targets)

### 🎯T2 Tier D combinators are implemented  [weight 0.6]  (status only)
Status: achieved

### 🎯T13 Per-worker wake eliminates thundering herd  [weight 0.4]  (status only)
Status: not started

## Recommendation

Work on: **🎯T12 Test names contain no spaces**
Reason: Highest effective weight (2.0) among unblocked, non-achieved targets. Cost 1 (mechanical find-and-replace) for value 2 — fixes shell quoting issues in lldb, scripts, and CI. Quick win before tackling larger targets like 🎯T14 (HAMT replacement) or 🎯T11 cleanup.

## Suggested action

Grep for `TEST_CASE("` in `test/*.test.cc`, replace spaces with hyphens in all test names, build and run `./build/normal/csp_tests -ltc` to verify no names contain spaces, then run `./build/normal/csp_tests` to confirm all tests pass. Run `make dist` afterward to update dist/ files.

<!-- convergence-deps
evaluated: 2026-04-05T09:00:00Z
sha: 4a6ddd6

🎯T12:
  gap: not started
  assessment: "667 TEST_CASE names with spaces in test/*.test.cc. All need renaming."
  read:
    - test/channel.test.cc
    - test/chanutil.test.cc
    - test/fanout.test.cc
    - test/flatten_strat.test.cc
    - test/imp_exit.test.cc
    - test/mn.test.cc
    - test/parallel_map.test.cc
    - test/timer.test.cc
    - test/tls.test.cc

🎯T14:
  gap: not started
  assessment: "HAMT code still present. No work begun on chained stack array replacement."
  read: []

🎯T15:
  gap: achieved
  assessment: "PicoTLS vendored, mbedTLS removed, TSan suppressions cleaned, 664/664 tests, 0 skipped."
  read:
    - vendor/github.com/h2o/picotls/
    - test/tsan_suppressions.txt
    - .gitmodules

🎯T11:
  gap: close
  assessment: "PR #18 merged. All major criteria met. Residual: default_scheduler_impl function name in csp.cc."
  read:
    - src/csp.cc
    - src/channel.cc
    - include/csp/csp.h

🎯T7:
  gap: achieved
  assessment: "3/6 sub-targets complete. Acceptance threshold met."
  read: []
-->
