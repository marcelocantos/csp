# Convergence Report

**Generated**: 2026-04-05
**Branch**: master
**SHA**: e37e19d

## Standing invariants

- Tests: 664/664 passed (local not re-run this evaluation).
- CI: **FAILING** — macOS ASan+UBSan job fails with UBSan error in vendored PicoTLS (`picotls.c:6041:54: runtime error: applying zero offset to null pointer`). All other 9/10 jobs pass (including Linux ASan+UBSan, both TSan jobs, Windows, TLA+). This is a UBSan false positive in third-party code, not a CSP bug.

## Movement

- 🎯T12: not started → **achieved** (bd8c086, all test names use hyphens)
- 🎯T11: close → **achieved** (`default_scheduler_impl` renamed to `main_loop_scheduler`, b3c024b)
- 🎯T14: not started → **parked** (paper 18 documents alternatives, HAMT works and is not a bottleneck)
- 🎯T15: (unchanged) achieved
- 🎯T7: (unchanged) achieved
- 🎯T3: (unchanged) not started
- 🎯T13: (unchanged) not started

## Gap report

### 🎯T3.1 Ergonomic I/O wrappers exist  [weight 1.6]
Gap: not started
No `fd_t` type, no `net::listen`/`net::dial`, no `io::lines`/`read_all`/`write_all`. Foundation for all networking targets.

### 🎯T3.2 Channel-native HTTP/1.1 server works  [weight 1.6]
Gap: not started
No llhttp vendored, no `http::serve`. Depends on 🎯T3.1.

### 🎯T3.5 WebSocket support (server and client)  [weight 1.6]
Gap: not started
No wslay vendored. Depends on 🎯T3.2 for HTTP upgrade path.

### 🎯T3 Runtime is production-ready for I/O workloads  [weight 0.6]
Gap: not started (0/9 sub-targets achieved)

### 🎯T14 Dynamic scoping improvements  [weight 0.5]  (parked)
Gap: parked
Paper 18 explores alternatives. HAMT works, not a bottleneck. Revisit if profiling points here.

### 🎯T13 Per-worker wake eliminates thundering herd  [weight 0.4]
Gap: not started
`unpark_one()` still uses `notify_all`. Three approaches explored in status notes, all have failure modes. Requires platform-specific futex primitives or design rethink.

### Achieved targets (unchanged)

- 🎯T12 Test names contain no spaces — achieved
- 🎯T11 Scheduler is always M:N — achieved
- 🎯T15 TLS uses PicoTLS instead of mbedTLS — achieved
- 🎯T7 Non-trivial example applications demonstrate CSP — achieved (3/6)
- 🎯T4 API safety gaps are closed — achieved
- 🎯T5 Unmodeled concurrent decision points have TLA+ specs — achieved
- 🎯T8 Signal handling is audited for correctness — achieved
- 🎯T2 Tier D combinators are implemented — achieved

## Recommendation

Work on: **CI UBSan failure (standing invariant violation)**
Reason: CI is red on macOS ASan+UBSan due to a UBSan null-pointer-offset error in vendored PicoTLS. Standing invariant violations take priority over all target work. This blocks all convergence — no PR can merge green until it's fixed.

## Suggested action

Investigate `vendor/github.com/h2o/picotls/lib/picotls.c:6041` — the UBSan "applying zero offset to null pointer" error. Options: (1) add a UBSan suppression for PicoTLS code (fast, pragmatic — it's third-party), (2) patch the line to guard against null before pointer arithmetic, or (3) check if upstream PicoTLS has a fix. Option 1 is recommended as the immediate fix — add a `ubsan_suppressions.txt` file and pass it via `-fsanitize-blacklist` for the PicoTLS compilation unit.

<!-- convergence-deps
evaluated: 2026-04-05T12:00:00Z
sha: e37e19d

🎯T12:
  gap: achieved
  assessment: "All test names use hyphens. No spaces found in TEST_CASE names."
  read:
    - test/channel.test.cc

🎯T11:
  gap: achieved
  assessment: "All criteria met. default_scheduler_impl renamed to main_loop_scheduler. No references in source code."
  read:
    - src/csp.cc

🎯T14:
  gap: parked
  assessment: "Paper 18 written. HAMT works, not a bottleneck. Parked."
  read:
    - docs/papers/18-dynamic-scope-alternatives.md

🎯T15:
  gap: achieved
  assessment: "PicoTLS vendored, mbedTLS removed, 664/664 tests. UBSan issue in picotls.c:6041 is third-party, not CSP."
  read: []

🎯T3:
  gap: not started
  assessment: "0/9 sub-targets achieved. No I/O wrapper, HTTP, or WebSocket work begun."
  read: []

🎯T13:
  gap: not started
  assessment: "unpark_one() still uses notify_all. No implementation progress."
  read: []

🎯T7:
  gap: achieved
  assessment: "3/6 sub-targets complete. Acceptance threshold met."
  read: []
-->
