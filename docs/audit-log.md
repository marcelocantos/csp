# Audit Log

Chronological record of audits, releases, documentation passes, and other
maintenance activities. Append-only — newest entries at the bottom.

## 2026-02-11 — initial-extraction (reconstructed)

- **Commit**: `d0dd0b8`
- **Outcome**: Initial extraction of CSP library from bricabrac monorepo.
  Brought over core M:1 scheduler, channel primitives (chan<T>, alt, prialt),
  timer channels, and ring buffer. README and CLAUDE.md established at creation.

## 2026-02-15 — infrastructure-pass (reconstructed)

- **Commit**: `cf5b484`
- **Outcome**: Added sanitizer support (ASan, UBSan, TSan) with fiber
  annotations, microbenchmarking infrastructure (nanobench), automatic header
  dependency tracking, and M:N threading (GMP model) for multi-core scheduling.
  Several bug fixes: channel memory leak, flaky timer test, alt() fairness via
  random offset.

## 2026-02-16 — api-refactor (reconstructed)

- **Commit**: `7ca04be`
- **Outcome**: Channel API overhauled: endpoints made move-only, `channel<T>`
  replaced with `chan<T>`, C-style extern "C" API layer replaced with
  `csp::internal` namespace, microthread.* renamed to csp.*, `#pragma once`
  adopted. Examples directory added with 12 runnable demos.

## 2026-02-17 — combinators-pass (reconstructed)

- **Commit**: `8bcaa83`
- **Outcome**: Extensive combinator library built out in `csp::part` namespace:
  merge, batch, scan, zip, flat_map, round_robin, interleave, slide/window,
  partition, reduce, gate, join, share, metrics, group_by, timer parts, plus
  kqueue-based CSP-aware I/O, blocking thread pool, and DNS resolution.

## 2026-02-18 — /docs (reconstructed)

- **Commit**: `95b943d`
- **Outcome**: Major documentation pass: guide chapters, reference catalog, and
  expanded architecture docs written. Agent guide (token-efficient) added.
  Documentation inaccuracies fixed across 13 files.

## 2026-02-19 — docs-update (reconstructed)

- **Commit**: `32e47c6`
- **Outcome**: Comprehensive CSP reference documentation added. Amalgamated
  distribution renamed from `amalg/` to `dist/`; AGENTS-CSP.md added to
  distribution. README slimmed down; markdown link checker added to `make test`.
  Documentation updated to reference amalgamated distribution.

## 2026-02-20 — combinators-tier-b (reconstructed)

- **Commit**: `2b06a67`
- **Outcome**: Tier B combinators added: take_until, any_of/all_of, chunk_by,
  foreach_emit, fallback, transpose, sort_merge. `parallel_map` added.
  "microthread" renamed to "imp" across entire codebase. `todo.md` added to
  docs.

## 2026-02-21 — channel-topology (reconstructed)

- **Commit**: `b1159d3`
- **Outcome**: Channel topology surgery primitives added: splice, weak refs, slot
  memory safety, tap (RAII observer with auto-fuse-back), fuse/split, 4-arg
  swap. TLA+ specs added for channel ops. SVG diagrams generated from ASCII-art
  DSL. Documentation written for swap, fuse, and tap.

## 2026-02-22 — cancellation-tls (reconstructed)

- **Commit**: `35733c9`
- **Outcome**: Cancel-aware TLS support added via mbedTLS vendor submodule.
  Timer/IO suspension unified via reactor signals with cancellation. Dynamic
  scoping (`csp::local`) and imp-local storage (`csp::mt_local<T>`) added.

## 2026-02-23 — cpp23-modernisation (reconstructed)

- **Commit**: `0b86c40`
- **Outcome**: Codebase modernised to C++23 idioms (spaceship operator,
  `[[nodiscard]]`, variable templates). Standard later downgraded back to C++20
  for broader compiler compatibility. `third_party/` layout migrated to
  `vendor/`.

## 2026-02-23 — release-v0.1.0 (reconstructed)

- **Commit**: `53c1ce8`
- **Outcome**: v0.1.0 tagged on the C++20 downgrade commit after the C++23
  modernisation pass was stabilised.

## 2026-02-25 — pre-release-hardening (reconstructed)

- **Commit**: `d993f3f`
- **Outcome**: Coverage-gap tests added across cancel, channel, clock, dynamic,
  io, mn, tls, and part modules. `cancel_op` renamed to `done`; multi-handle
  join overloads added. NOTICES file added for third-party licence attribution.
  STABILITY.md added to track pre-1.0 API surface.

## 2026-02-26 — release-v0.2.0 (reconstructed)

- **Commit**: `8224419`
- **Outcome**: v0.2.0 tagged on the STABILITY.md addition commit. `CSP_VERSION`
  macros added to `csp.h`. Imp death interception via dynamic scope (`imp_exit`)
  implemented. `CHECK_*/REQUIRE_*` macros replaced with plain `CHECK/REQUIRE`.
  Task tracking added to CLAUDE.md.

## 2026-02-27 — ci-setup (reconstructed)

- **Commit**: `d7e6d64`
- **Outcome**: GitHub Actions CI workflow added with sanitizer and
  cross-platform matrix (macOS/Linux, clang/gcc). Build artifacts moved under
  `build/`. Non-template implementations moved from headers to `.cc` files.

## 2026-03-27 — /audit signal handling (🎯T8)

- **Commit**: (this commit)
- **Outcome**: Signal handling code reviewed for async-signal-safety. No
  violations found.

### Unix signal handler (`src/signal.cc:42-52`)

The handler calls only async-signal-safe operations:
- `std::atomic<int>::load(acquire)` — lock-free atomic on integral type
- `std::atomic<uint64_t>::load(acquire)` — lock-free atomic
- `::write()` — POSIX async-signal-safe

No mutex, no malloc, no printf, no C++ exceptions.

**Ordering correctness**: `write_fd` is written before `g_sig_pipe_count`
is incremented with release. Handler loads count with acquire, so
`write_fd` is visible by the time the handler sees the new count.
Verified by `formal/SignalPipeLifecycle.tla`.

**Teardown race (benign)**: Between the handler loading `sig_mask` (non-zero)
and calling `write()`, the sentinel imp can clear the mask and close the fd.
The `write()` then hits EBADF and the byte is lost. This is benign: the
signal was being torn down, and the byte would have been ignored anyway.
On Linux, the closed-pipe `write()` would deliver SIGPIPE, but the runtime
ignores SIGPIPE process-wide (v0.5.0 fix).

### Windows console handler (`src/win_signal.cc:41-56`)

The handler runs on a normal OS thread (not async-signal context), so all
APIs are safe. Uses the same atomic-guard pattern as Unix for consistency.

### Deferred

None.

## 2026-03-24 — /release v0.5.0

- **Commit**: (pending)
- **Outcome**: Released v0.5.0. Fixed process shutdown crash (Runtime + Reactor
  thread join) and TSan false positives (mbedTLS suppression file). 11/11 CI
  jobs pass. Added ETL pipeline and task scheduler examples. 641/641 tests.

## 2026-03-22 — /release v0.4.0

- **Commit**: `d086cf0`
- **Outcome**: Released v0.4.0. Request/response primitives (`request<Req, Resp>`,
  `call()`, callable writer endpoints), pipe operator (`w | r` for fuse),
  bidirectional lifecycle observability named as design principle. Linux SIGPIPE
  fix, TSan race fix, stack analysis sanitizer guard. STABILITY.md updated with
  5 new Fluid API items. 641/641 tests pass.

## 2026-03-08 — /release v0.3.0

- **Commit**: `62f732a`
- **Outcome**: Released v0.3.0. Windows port (PR #4), buffered channels, imp
  exit/supervision, GitHub Actions CI (11-job matrix), platform-neutral I/O.
  STABILITY.md updated (resolved version macros gap, added new symbols,
  updated out-of-scope items). 636/636 tests pass on all platforms.

## 2026-02-28 — platform-expansion (reconstructed)

- **Commit**: `8167ab9`
- **Outcome**: Windows port Phase 1 (CMake, platform guards, VirtualAlloc stack
  pool) and Phase 2 (CreateThreadpoolTimer-based reactor) completed. Linux epoll
  reactor backend added. 20 demo programs added. Worker threads labelled
  `csp-1`, `csp-2`, etc. for clearer diagnostics.

## 2026-04-06 — /release v0.7.0

- **Commit**: `0a49625`
- **Outcome**: Released v0.7.0. Opaque `fd_t` type wrapping raw file descriptors
  (no implicit int conversion). All CSP fd-producing functions return `fd_t`
  already set non-blocking. New convenience functions: `io::read_all`,
  `io::write_all`, `part::io::lines`, `file::read`/`file::write` (blocking
  pool). `byte_reader`/`byte_writer` assert non-blocking. `fd_t` threaded
  through net, TLS, signals, examples. 668/668 tests (4 new).
  STABILITY.md updated for v0.7.0. 🎯T3.1 achieved.

## 2026-04-05 — /release v0.6.0

- **Commit**: `c0b4c2f` (PR #18)
- **Outcome**: Released v0.6.0. Major changes: M:N-only scheduler (single-P mode
  removed), PicoTLS swap (mbedTLS → PicoTLS minicrypto, TLS 1.3 only, fiber-safe),
  net::listen TCP half-close fix, all tracked bugs closed (supervisor SIGSEGV,
  push_to_global assertion, mbedTLS TSan false races, cancel/timer flakes).
  664/664 tests, 0 skipped. All 10 CI jobs green. New targets: 🎯T12–T15.
  STABILITY.md updated for v0.6.0 (TLS API surface changes).

## 2026-04-06 — /release v0.8.0

- **Commit**: `880b952`
- **Outcome**: Released v0.8.0. Three new example applications: sensor fusion
  (combine_latest, sample, window anomaly detection), web crawler (BFS, worker
  pool, per-host rate limiting, BLO shutdown), log aggregator (merge, severity
  routing, tick windows, alerting). Direct-pointer channel transfer optimization:
  move-writes skip staging buffer, one fewer move per exchange; unmatched alt
  arms no longer waste moves. 668/668 tests. 🎯T7.3, 🎯T7.4, 🎯T7.6 achieved
  (🎯T7 now 6/6). STABILITY.md updated.
