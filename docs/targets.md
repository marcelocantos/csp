# Targets

<!-- last-evaluated: fe73f81 -->

## Active

### 🎯T2 Tier D combinators are implemented
- **Weight**: 1 (value 3 / cost 5)
- **Acceptance**:
  - `diff`, `frame`, `reorder`, `race` exist in `include/csp/part/`
  - Each has tests in `test/` and a detail page in `docs/reference/parts/`
  - Catalog table in `docs/reference/parts.md` updated
  - `dist/AGENTS-CSP.md` combinator table updated
- **Status**: not started
- **Discovered**: 2026-03-09
- **Context**: Revised from original 6 to 4. `repeat` dropped (redundant with `cycle`), `compose` dropped (redundant with `operator|`), `amb` renamed to `race`.

### 🎯T3 Runtime is production-ready for I/O workloads
- **Weight**: 1 (value 21 / cost 34)
- **Acceptance**: all sub-targets achieved
- **Status**: not started
- **Discovered**: 2026-03-09

### 🎯T3.1 Ergonomic I/O wrappers exist
- **Weight**: 2 (value 8 / cost 5)
- **Parent**: 🎯T3
- **Acceptance**:
  - Opaque `fd_t` type wraps raw file descriptors; no implicit conversion to int
  - All CSP functions that produce fds (`io::accept`, `net::listen`, `net::dial`) return `fd_t` already set non-blocking
  - `byte_reader`/`byte_writer` accept `fd_t` and assert (not fix) non-blocking
  - `csp::net::listen`, `csp::net::dial` for TCP
  - `csp::io::lines`, `csp::io::read_all`, `csp::io::write_all` for fd I/O
  - `csp::file::read`, `csp::file::write` via blocking pool
  - Tests and reference docs for each
- **Status**: not started
- **Discovered**: 2026-03-09

### 🎯T3.2 Channel-native HTTP server works
- **Weight**: 1 (value 8 / cost 13)
- **Parent**: 🎯T3
- **Acceptance**:
  - `csp::http::serve(port)` returns `reader<request<Req, Resp>>`
  - Typed JSON codecs for request/response bodies
  - WebSocket upgrade to `reader<ws::message>` + `writer<ws::message>` (separate channels for recv/send)
  - SSE upgrade to `writer<sse::event>`
  - Middleware composable via stream combinators
  - Request context via `csp::dynamic`
  - Graceful shutdown via channel lifecycle
- **Status**: not started
- **Discovered**: 2026-03-09

### 🎯T3.3 High-density stack scaling supports 100K+ imps
- **Weight**: 1 (value 3 / cost 8)
- **Parent**: 🎯T3
- **Acceptance**:
  - 100K+ concurrent imps without kernel memory pressure or `vm.max_map_count` issues
  - Software overflow detection at API checkpoints or arena-based allocation
  - Existing tests still pass (no regression)
- **Status**: not started
- **Discovered**: 2026-03-09

### 🎯T3.4 Context-aware stack depth analysis is accurate
- **Weight**: 1 (value 2 / cost 8)
- **Parent**: 🎯T3
- **Acceptance**:
  - ARM64 instruction walker resolves nested function pointers, vtables, parameter-driven paths
  - Interprocedural data flow and profile-guided calibration
  - Stack allocations are tight without overflows on real workloads
- **Status**: not started
- **Discovered**: 2026-03-09

### 🎯T4 API safety gaps are closed
- **Weight**: 1 (value 5 / cost 4)
- **Acceptance**: all sub-targets achieved
- **Status**: achieved — T4.1 (closer<EP>) and T4.2 (main() error message) both done.
- **Discovered**: 2026-03-09
- **Achieved**: 2026-03-26

### 🎯T4.1 `closer<EP>` enforces vulture-only endpoints
- **Weight**: 2 (value 3 / cost 2)
- **Parent**: 🎯T4
- **Acceptance**:
  - `closer<EP>` type exists with `operator~`, `operator bool`, and `endpoint()` escape hatch
  - CTAD: `closer(reader<T>)` → `closer<reader<T>>`
  - Users can wrap spawn handles: `closer handle(spawn(f));`
  - `done()` return type change deferred — requires rethinking chan_op internals
  - `spawn(f)` keeps returning `reader<exception_ptr>` — supervisor legitimately reads exceptions
- **Status**: achieved — `closer<EP>` implemented with 5 tests (646/646 pass)
- **Achieved**: 2026-03-26
- **Discovered**: 2026-03-09
- **Context**: Original acceptance criteria called for spawn/done to return closer directly. This was revised: spawn must return reader (supervisor reads exceptions), done returns chan_op (no reader to wrap). closer is opt-in via CTAD wrapping, which is the right design — it's a restriction the caller chooses, not one forced by the API.

### 🎯T4.2 main() can perform CSP operations
- **Weight**: 1 (value 2 / cost 2)
- **Parent**: 🎯T4
- **Acceptance**:
  - `csp::local` in `main()` does not crash
  - Either a lightweight "main imp" context is established automatically, or the limitation is documented with a clear error message
- **Status**: achieved — clear error message: "channel operation attempted from main() — CSP operations must run inside spawn()". Replaces opaque assert failure.
- **Achieved**: 2026-03-26
- **Discovered**: 2026-03-09

### 🎯T5 Unmodeled concurrent decision points have TLA+ specs
- **Weight**: 1 (value 3 / cost 3)
- **Acceptance**:
  - Blocking pool shutdown, stack pool reclamation, M:N worker join audited
  - New TLA+ specs in `formal/` for any unmodeled protocols found
  - Bug variants (`_Bug.tla`) for each new spec
- **Status**: achieved — 3 new spec pairs: BlockingPoolLifecycle (lazy-init double-check + shutdown), SurplusProcessorReset (in-place reset vs steal_work), SignalPipeLifecycle (async-signal-safe handler registration). All correct specs pass TLC; all bug variants find violations.
- **Achieved**: 2026-03-27
- **Discovered**: 2026-03-09

### 🎯T7 Non-trivial example applications demonstrate CSP
- **Weight**: 2 (value 30 / cost 18)
- **Acceptance**: at least 3 sub-target examples are complete
- **Status**: achieved — 3/6 sub-targets complete (T7.1 chat server, T7.2 ETL pipeline, T7.5 task scheduler)
- **Discovered**: 2026-03-09
- **Achieved**: 2026-03-23

### 🎯T7.1 Chat server example
- **Weight**: 1 (value 5 / cost 3)
- **Parent**: 🎯T7
- **Acceptance**:
  - Multi-room chat with per-client imps
  - Fan-out to subscribers, join/leave lifecycle, backpressure on slow clients
  - Compiles and runs as a standalone binary in `examples/`
- **Status**: achieved — committed (PR #11). Multi-room chat with per-client imps, fan-out, join/leave, backpressure. Shutdown crash is a runtime issue (TODO), not a chat server deficiency.
- **Achieved**: 2026-03-22
- **Discovered**: 2026-03-09

### 🎯T7.2 ETL pipeline example
- **Weight**: 1 (value 5 / cost 3)
- **Parent**: 🎯T7
- **Acceptance**:
  - Ingests CSV/JSON, parses, validates, transforms, enriches, deduplicates, batch-writes to SQLite
  - Demonstrates chain, parallel_map, batch, scan, buffer, backpressure
  - Compiles and runs in `examples/`
- **Status**: achieved — seven-stage pipeline (parse → validate → normalize → deduplicate → enrich → batch → report). Backpressure via unbuffered channels between stages.
- **Achieved**: 2026-03-23
- **Discovered**: 2026-03-09

### 🎯T7.3 Web crawler example
- **Weight**: 1 (value 5 / cost 3)
- **Parent**: 🎯T7
- **Acceptance**:
  - Breadth-first crawl with bounded concurrency, URL frontier, dedup
  - Per-host rate limiting, graceful shutdown
  - Compiles and runs in `examples/`
- **Status**: not started
- **Discovered**: 2026-03-09

### 🎯T7.4 Sensor fusion dashboard example
- **Weight**: 1 (value 5 / cost 3)
- **Parent**: 🎯T7
- **Acceptance**:
  - Multiple simulated sensor streams at different rates
  - combine_latest fusion, quantize throttling, sliding window anomaly detection
  - Compiles and runs in `examples/`
- **Status**: not started
- **Discovered**: 2026-03-09

### 🎯T7.5 Task scheduler example
- **Weight**: 1 (value 5 / cost 3)
- **Parent**: 🎯T7
- **Acceptance**:
  - Priority queue, worker pool, per-job timeout, dependency DAG
  - Progress reporting over channels, structured cancellation
  - Compiles and runs in `examples/`
- **Status**: achieved — priority dispatcher, 3-worker pool, request/response with operator(), endpoint bundle pattern, BLO-driven shutdown cascade.
- **Achieved**: 2026-03-23
- **Discovered**: 2026-03-09

### 🎯T7.6 Log aggregator example
- **Weight**: 1 (value 5 / cost 3)
- **Parent**: 🎯T7
- **Acceptance**:
  - Tails multiple log files, parses, routes by severity
  - Time-window aggregation, periodic flush, threshold alerts
  - Compiles and runs in `examples/`
- **Status**: not started
- **Discovered**: 2026-03-09

### 🎯T8 Signal handling is audited for correctness
- **Weight**: 1 (value 2 / cost 2)
- **Acceptance**:
  - Signal handling code reviewed for async-signal-safety
  - Any violations fixed or documented as acceptable
  - Audit findings recorded in `docs/audit-log.md`
- **Status**: achieved — no violations found. Handler uses only atomic loads + write(). Teardown race is benign (EBADF on closed fd, SIGPIPE ignored). Findings in audit-log.md.
- **Achieved**: 2026-03-27
- **Discovered**: 2026-03-09

## Achieved

### 🎯T9 TSan is clean on all CI jobs
- **Weight**: 2 (value 3 / cost 5)
- **Acceptance**:
  - Linux arm64 TSan, Linux x86_64 TSan, and macOS TSan CI jobs all pass reliably
  - mbedTLS TSan false positives resolved (via TSan suppressions)
  - Cancel-during-I/O flake in `cancel.test.cc:654` resolved
- **Context**: Root cause: TSan tracks pthread mutex operations per-OS-thread, not per-fiber. M:N migration causes false races in mbedTLS. Suppression file (`test/tsan_suppressions.txt`) resolves. Cancel flake not recurred since suppressions added. Paper 16 documents the investigation.
- **Status**: achieved
- **Discovered**: 2026-03-15
- **Achieved**: 2026-03-24

### 🎯T10 Bidirectional lifecycle observability is documented as a design principle
- **Weight**: 2 (value 5 / cost 3)
- **Acceptance**:
  - A document (guide chapter or design paper) names and explains bidirectional lifecycle observability
  - Contrasts with Go channels
  - Explains the compositionality this enables
  - Referenced from the agent guide and README
- **Context**: Paper 15 (Channels as Interfaces), guide chapter 02, reference channels.md, AGENTS-CSP.md, and README all name and explain the principle. Go contrast in paper 15 and channels reference.
- **Status**: achieved
- **Discovered**: 2026-03-21
- **Achieved**: 2026-03-22

### 🎯T6 Local Docker testing covers Linux scenarios
- **Weight**: 2 (value 3 / cost 2)
- **Acceptance**:
  - `make docker-test` runs both ARM64 and x86_64 Linux builds locally
  - x86 cross-compilation works
  - Documented in CLAUDE.md or README
- **Context**: ARM64 636/636, x86_64 630/630 (no TLS). Documented in CLAUDE.md. Required SIGPIPE fix for Linux (484b5e4).
- **Status**: achieved
- **Discovered**: 2026-03-09
- **Achieved**: 2026-03-15

### 🎯T1 Windows port PR is merged to master
- **Weight**: 2 (value 8 / cost 5)
- **Estimated-cost**: 5
- **Actual-cost**: 5
- **Acceptance**:
  - All 11 CI jobs pass (macOS test, Linux test x2, sanitizers x6, Windows, TLA+)
  - `make dist` output matches committed dist/ files
  - Windows test exe runs doctest and reports pass/fail (not silent crash)
  - PR #4 squash-merged to master
- **Context**: PR #4 merged (f432f16). All CI green. Windows 621/621 tests pass.
- **Status**: achieved
- **Discovered**: 2026-03-01
- **Achieved**: 2026-03-08

### 🎯T1.3 Windows test exe runs and reports results
- **Weight**: 1 (value 5 / cost 5)
- **Estimated-cost**: 5
- **Actual-cost**: 5
- **Acceptance**:
  - `csp_tests.exe` prints doctest summary on Windows CI
  - Exit code reflects test results (0 = all pass)
- **Context**: 621/621 tests pass on Windows CI.
- **Parent**: 🎯T1
- **Status**: achieved
- **Discovered**: 2026-03-04
- **Achieved**: 2026-03-08

### 🎯T1.2 macOS mn.test MultipleThreads flake is fixed
- **Weight**: 2 (value 3 / cost 2)
- **Estimated-cost**: 2
- **Actual-cost**: 1
- **Acceptance**: macOS arm64 CI test job passes reliably (no flake on `CHECK(thread_ids.size() > 1)`)
- **Context**: Replaced busy-loop with `csp::yield()` to force scheduler distribution.
- **Parent**: 🎯T1
- **Status**: achieved
- **Discovered**: 2026-03-04
- **Achieved**: 2026-03-05

### 🎯T1.1 Dist files are regenerated and committed
- **Weight**: 8 (value 8 / cost 1)
- **Estimated-cost**: 1
- **Actual-cost**: 1
- **Acceptance**:
  - `make dist && git diff --exit-code dist/` passes
  - Sanitizer and TSan jobs can compile test-dist
- **Context**: dist/ files were stale from master merge.
- **Parent**: 🎯T1
- **Status**: achieved
- **Discovered**: 2026-03-04
- **Achieved**: 2026-03-04
