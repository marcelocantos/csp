# Convergence Report

**Generated**: 2026-04-03
**Branch**: mn-only-scheduler-net-listen-fix
**SHA**: 70ef67b5478c7bf28e8ef17d7339bfeaaea1b863

## Standing invariants

- Tests: 663/663 passed (branch status).
- CI: pending (PR #18 open).

## Movement

- 🎯T7: converging → achieved (T7.1, T7.2, T7.5 complete).
- 🎯T4: not started → achieved.
- 🎯T5: not started → achieved.
- 🎯T8: not started → achieved.
- 🎯T2: not started → achieved.
- 🎯T9: not started → achieved (moved to Achieved section).
- 🎯T6: close → achieved.
- 🎯T10: not started → achieved.
- 🎯T1: achieved (unchanged).
- New targets: 🎯T11 (in progress), 🎯T12 (not started), 🎯T13 (not started).

## Gap report

### 🎯T12 Test names contain no spaces  [weight 2.0]
Gap: not started
Grep shows 668 TEST_CASE names with spaces in test/*.test.cc. All need renaming to use hyphens or underscores.

### 🎯T7 Non-trivial example applications demonstrate CSP  [weight 1.7]
Gap: achieved
3/6 sub-targets complete (T7.1 chat server, T7.2 ETL pipeline, T7.5 task scheduler). Acceptance met (at least 3).

### 🎯T11 Scheduler is always M:N  [weight 1.6]
Gap: close
Core scheduler done, PR #18 open with 663/663 tests pass. Remaining: fake_clock auto-advance via main_loop quiescence hook, inline `fake_clock{}` syntax.

### 🎯T4 API safety gaps are closed  [weight 1.2]  (status only)
Status: achieved

### 🎯T5 Unmodeled concurrent decision points have TLA+ specs  [weight 1.0]  (status only)
Status: achieved

### 🎯T8 Signal handling is audited for correctness  [weight 1.0]  (status only)
Status: achieved

### 🎯T3 Runtime is production-ready for I/O workloads  [weight 0.6]  (status only)
Status: not started (0/9 sub-targets).

### 🎯T2 Tier D combinators are implemented  [weight 0.6]  (status only)
Status: achieved

### 🎯T13 Per-worker wake eliminates thundering herd  [weight 0.4]  (status only)
Status: not started

## Recommendation

Work on: **🎯T12 Test names contain no spaces**
Reason: Highest weight (2.0) among unblocked targets. Small cost (1) for high value (2) — fixes shell quoting issues in lldb, scripts, and CI without affecting functionality.

## Suggested action

Grep for TEST_CASE(".* .*") in test/*.test.cc, replace spaces with hyphens or underscores, verify ./build/normal/csp_tests -ltc shows no spaces, and -tc= filters work without quoting.

<!-- convergence-deps
evaluated: 2026-04-03T14:30:00Z
sha: 70ef67b5478c7bf28e8ef17d7339bfeaaea1b863

🎯T12:
  gap: not started
  assessment: "Grep shows 668 TEST_CASE names with spaces in test/*.test.cc. All need renaming to use hyphens or underscores."
  read:
    - test/tls.test.cc
    - test/cancel.test.cc
    - test/channel.test.cc
    - test/timer.test.cc
    - test/imp_exit.test.cc

🎯T7:
  gap: achieved
  assessment: "3/6 sub-targets complete (T7.1 chat server, T7.2 ETL pipeline, T7.5 task scheduler). Acceptance met (at least 3)."
  read:

🎯T11:
  gap: close
  assessment: "Core scheduler done, PR #18 open with 663/663 tests pass. Remaining: fake_clock auto-advance via main_loop quiescence hook, inline fake_clock{} syntax."
  read:
-->
</content>
<parameter name="filePath">docs/convergence-report.md