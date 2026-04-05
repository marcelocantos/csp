# Targets

<!-- last-evaluated: b9983a0 -->

## Active

### 🎯T2 Tier D combinators are implemented
- **Weight**: 1 (value 3 / cost 5)
- **Acceptance**:
  - `diff`, `frame`, `reorder`, `race` exist in `include/csp/part/`
  - Each has tests in `test/` and a detail page in `docs/reference/parts/`
  - Catalog table in `docs/reference/parts.md` updated
  - `dist/AGENTS-CSP.md` combinator table updated
- **Status**: achieved — diff, frame, reorder, race implemented with 17 tests. 663/663 pass.
- **Achieved**: 2026-03-28
- **Discovered**: 2026-03-09
- **Context**: Revised from original 6 to 4. `repeat` dropped (redundant with `cycle`), `compose` dropped (redundant with `operator|`), `amb` renamed to `race`.

### 🎯T3 Runtime is production-ready for I/O workloads
- **Weight**: 1 (value 21 / cost 34)
- **Acceptance**: Core: T3.1 (I/O wrappers) + T3.2 (HTTP/1.1) + T3.5 (WebSocket) + T3.6 (HTTP client). Full stack: + T3.7 (HTTP/2) + T3.8 (QUIC) + T3.9 (HTTP/3). Infrastructure: T3.3 (stack scaling), T3.4 (stack analysis).
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
- **Status**: close — `io::socket_t`, `net::listen`/`net::dial`, `byte_reader`/`byte_writer`, `split_lines` exist. Missing `fd_t` opaque wrapper, `read_all`/`write_all`, `file::read`/`file::write`.
- **Discovered**: 2026-03-09

### 🎯T3.2 Channel-native HTTP/1.1 server works
- **Weight**: 2 (value 8 / cost 5)
- **Parent**: 🎯T3
- **Acceptance**:
  - llhttp vendored in `vendor/` for HTTP/1.1 parsing
  - `csp::http::serve(port)` returns a reader of per-connection endpoint bundles
  - Each connection exposes `reader<http::request>` + `writer<http::response>`
  - Request/response types carry headers, method, path, body as channels (streaming body support)
  - Keep-alive handled automatically
  - Graceful shutdown via channel lifecycle (drop the listener → connections drain)
  - Tests and reference docs
- **Status**: not started
- **Discovered**: 2026-03-09
- **Context**: Decomposed from original T3.2 which was overloaded. llhttp (Node.js parser) is parser-only, no I/O — integrates cleanly as a filter between byte_reader and structured output.

### 🎯T3.5 WebSocket support (server and client)
- **Weight**: 2 (value 8 / cost 5)
- **Parent**: 🎯T3
- **Acceptance**:
  - wslay vendored in `vendor/` for WebSocket frame parsing/serialization
  - Server: `ws::upgrade(http_conn)` returns `reader<ws::message>` + `writer<ws::message>`
  - Client: `ws::connect(url)` returns the same endpoint pair
  - Ping/pong handled automatically
  - Close handshake via endpoint death (BLO: drop writer → close frame sent → reader sees death)
  - Binary and text message types
  - Tests and reference docs
- **Status**: not started
- **Discovered**: 2026-03-28
- **Context**: wslay is parser-only (MIT, ~2K lines). No I/O — feed bytes, get frames. Maps to a filter combinator over raw fd I/O. Depends on 🎯T3.2 for HTTP upgrade path.

### 🎯T3.6 HTTP client
- **Weight**: 1 (value 5 / cost 5)
- **Parent**: 🎯T3
- **Acceptance**:
  - `csp::http::get(url)` / `post(url, body)` return `reader<http::response>` (non-blocking)
  - Connection pooling with per-host channels
  - TLS support via PicoTLS integration
  - Timeout via cancellation scope
  - Tests and reference docs
- **Status**: not started
- **Discovered**: 2026-03-28
- **Context**: Reuses llhttp for response parsing. Reuses `net::dial` from 🎯T3.1 for outbound connections. The client is simpler than the server — no connection accept loop, just dial + parse.

### 🎯T3.7 HTTP/2 support
- **Weight**: 1 (value 5 / cost 8)
- **Parent**: 🎯T3
- **Acceptance**:
  - nghttp2 vendored in `vendor/` for HTTP/2 session management
  - Server: `http2::serve(port)` with multiplexed streams as independent channel bundles
  - Each stream is a `reader<http::request>` + `writer<http::response>` (same types as HTTP/1.1)
  - Server push via explicit stream creation
  - HPACK header compression handled by nghttp2
  - Flow control maps to channel backpressure
  - ALPN negotiation for TLS (h2 vs http/1.1)
  - Tests and reference docs
- **Status**: not started
- **Discovered**: 2026-03-28
- **Context**: nghttp2 has a "bring your own I/O" API (nghttp2_session_mem_recv/send). CSP owns the fds, nghttp2 owns the protocol state. HTTP/2 multiplexing maps naturally to CSP — each stream is an imp with its own channel pair. Depends on 🎯T3.2 (shared HTTP types) and 🎯T3.1 (I/O wrappers).

### 🎯T3.8 QUIC transport
- **Weight**: 1 (value 5 / cost 13)
- **Parent**: 🎯T3
- **Acceptance**:
  - ngtcp2 vendored in `vendor/` for QUIC protocol
  - `csp::quic::listen(port)` returns reader of connection bundles
  - `csp::quic::dial(addr)` returns a connection bundle
  - Each QUIC connection exposes stream creation: `conn.open_stream()` returns bidirectional channel pair
  - 0-RTT connection establishment
  - Connection migration (IP change) transparent to imp code
  - Integrates with CSP's reactor (UDP socket events)
  - Tests and reference docs
- **Status**: not started
- **Discovered**: 2026-03-28
- **Context**: ngtcp2 (by nghttp2's author) is designed for external event loop integration. QUIC uses UDP (not TCP), so reactor needs to handle UDP socket readability. Each QUIC stream maps to a channel pair — multiplexing is native to both QUIC and CSP. This is the largest networking target. Depends on 🎯T3.1 (I/O wrappers).

### 🎯T3.9 HTTP/3 over QUIC
- **Weight**: 1 (value 5 / cost 5)
- **Parent**: 🎯T3
- **Acceptance**:
  - nghttp3 vendored in `vendor/` for HTTP/3 framing
  - Server: `http3::serve(port)` with same request/response types as HTTP/1.1 and HTTP/2
  - Client: `http3::get(url)` / `post(url, body)` with same response type
  - QPACK header compression handled by nghttp3
  - Applications can serve HTTP/1.1, HTTP/2, and HTTP/3 from the same handler code (protocol-agnostic request/response types)
  - Tests and reference docs
- **Status**: not started
- **Discovered**: 2026-03-28
- **Context**: nghttp3 sits on top of ngtcp2 (🎯T3.8) the same way nghttp2 sits on top of TCP. The key design goal is protocol-agnostic handler code: a handler that reads `http::request` and writes `http::response` should work unchanged across HTTP/1.1, HTTP/2, and HTTP/3. The protocol differences are hidden below the channel layer. Depends on ��T3.7 (shared HTTP/2 types) and 🎯T3.8 (QUIC transport).

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

### 🎯T14 Dynamic scoping improvements
- **Weight**: 0.5 (value 3 / cost 6)
- **Status**: parked (2026-04-05)
- **Discovered**: 2026-04-04
- **Context**: HAMT (231 lines) works and the paper-09 bugs are fixed. Not a
  current bottleneck. Several alternative designs were explored and parked —
  see `docs/papers/18-dynamic-scope-alternatives.md` for the full analysis.
  Revisit if profiling points here or if the HAMT causes new issues.

### 🎯T13 Per-worker wake eliminates thundering herd
- **Weight**: 1 (value 3 / cost 8)
- **Acceptance**:
  - `unpark_one()` wakes exactly one sleeping worker, not all
  - No `notify_all` on shared condvar in the imp scheduling hot path
  - main_loop quiescence detection still works (fake_clock tests pass)
  - 665/665 tests pass, no performance regression
- **Status**: not started
- **Discovered**: 2026-04-02
- **Context**: Current `unpark_one()` does `park_cv.notify_all()` on a shared condvar, waking all parked workers (thundering herd). Investigation found three approaches and their failure modes: (1) per-P condvar deadlocks with `has_work()` predicate holding `global_mu`; (2) `atomic::wait` avoids deadlock but main_loop quiescence ping-pongs on every park cycle; (3) split worker/main condvar has lost-wakeup race. The proper fix requires either platform-specific futex primitives (`__ulock_wait`/`futex`), Go-style `note` (single-word futex per M), or decoupling main_loop quiescence from the worker parking condvar. See Go runtime (`gopark`/`goready`/`notesleep`) and Tokio's per-worker deque + atomic searching counter for reference designs.

### 🎯T12 Test names contain no spaces
- **Weight**: 1 (value 2 / cost 1)
- **Acceptance**:
  - All TEST_CASE names use hyphens or underscores instead of spaces
  - `./build/normal/csp_tests -ltc` shows no names with spaces
  - `-tc=` filters work without quoting gymnastics
- **Status**: achieved (2026-04-05, bd8c086)
- **Discovered**: 2026-04-02
- **Context**: Spaces in test names cause shell quoting issues with doctest's `-tc=` filter, especially inside lldb, scripts, and CI. All 667 names renamed to use hyphens.

### 🎯T11 Scheduler is always M:N
- **Weight**: 3 (value 8 / cost 5)
- **Acceptance**:
  - `mn_mode_` flag removed; always M:N with min 2 procs
  - `schedule()` renamed to `await_completion()` (`schedule()` kept as deprecated inline alias)
  - `run()` removed from public API or reimplemented via quiescence detection (parks until workers finish or deadlock detected)
  - `use_run` removed from channel matching (always `peer->schedule()`)
  - Daemon imp concept: `daemon_gs` counter excludes daemon imps from completion check
  - `RunStats` exception handler spawned as daemon
  - `fake_clock` reworked to detect quiescence without cooperative `run()` loop
  - All 665+ tests pass
  - `default_scheduler_impl` deleted; scheduler always uses `main_loop()`
- **Status**: achieved (2026-04-05) — PR #18 merged (v0.6.0). All criteria met: mn_mode_ removed, await_completion() exists, use_run gone, daemon imps, fake_clock quiescence rework done, `default_scheduler_impl` renamed to `main_loop_scheduler`.
- **Discovered**: 2026-03-29
- **Context**: M:N scheduler complete. `quiescence_scope` implemented for deterministic testing (scoped, inheritable via Imp::qs_). fake_clock thread-safe (mutex on pending_). fake_clock auto-advance via main_loop quiescence hook done.

### 🎯T15 TLS uses PicoTLS instead of mbedTLS
- **Weight**: 3 (value 5 / cost 3)
- **Acceptance**:
  - mbedTLS submodule removed, PicoTLS vendored at `vendor/github.com/h2o/picotls/`
  - minicrypto backend (no OpenSSL dependency)
  - TLS 1.3 only (documented limitation)
  - No built-in X.509 chain verification; `context::set_verify_callback()` hook for user-supplied verification
  - Test certs regenerated as ECDSA secp256r1 (minicrypto requirement)
  - Linux concurrent connections test unskipped (no more pthread mutex issue)
  - TSan suppressions for mbedTLS removed
  - All TLS tests pass on macOS and Linux
  - CLAUDE.md, AGENTS-CSP.md, dist/ updated
- **Status**: achieved — PicoTLS vendored, mbedTLS removed, 664/664 tests pass (0 skipped), all CI green including Linux TSan/ASan.
- **Achieved**: 2026-04-05
- **Discovered**: 2026-04-04
- **Context**: mbedTLS uses internal pthread mutexes that cause SIGABRT under M:N fiber migration on Linux (lock on thread T1, unlock on T2). TSan also reports false races. PicoTLS has zero internal locking — thread-local PRNG only, harmless under migration. Buffer-in/buffer-out API integrates cleanly with CSP's reactor. minicrypto backend is self-contained (cifra + micro-ecc, ~13.5K lines C). Limitation: minicrypto only supports secp256r1 keys and has no X.509 verifier. TLS 1.2 fallback can be added later via a second backend if needed.

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
