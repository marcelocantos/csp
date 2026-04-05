# Convergence Report

**Generated**: 2026-04-06
**Branch**: master
**SHA**: b9983a0

## Standing invariants

- Tests: 664/664 passed (per last session).
- CI: **GREEN** (9/10 jobs passed on 454bea8; macOS arm64 test job was cancelled, not failed. macOS ASan+UBSan now passes after UBSan fix 608c5dc. PR branch 05ab580 was 10/10 green).

## Movement

- CI: **FAILING** (UBSan) → **GREEN** (UBSan fix 608c5dc merged via PR #19)
- All targets: (unchanged since last report)

## Gap report

### 🎯T7.3 Web crawler example  [weight 1.7]
Gap: not started
No web crawler in `examples/`. Depends on networking infrastructure (which partially exists via `net::listen`/`net::dial`), but could be built as a simulated/mock example like the existing examples.

### 🎯T7.4 Sensor fusion dashboard example  [weight 1.7]
Gap: not started
No sensor fusion example in `examples/`. Pure channel-based — no I/O dependencies. Uses `combine_latest`, `quantize`, sliding windows.

### 🎯T7.6 Log aggregator example  [weight 1.7]
Gap: not started
No log aggregator in `examples/`. Could use blocking pool for file tailing, channels for routing/aggregation.

### 🎯T3.1 Ergonomic I/O wrappers exist  [weight 1.6]
Gap: **close**
Significant infrastructure already exists but doesn't match the target's naming/shape:
- `io::socket_t` exists (not `fd_t`, but functionally equivalent)
- `net::listen` and `net::dial` exist and return `connection` with split I/O channels
- `byte_reader`/`byte_writer` exist in `csp::part::io` (but call `set_nonblock` instead of asserting)
- `split_lines` exists (equivalent to `io::lines`)
- Missing: `io::read_all`/`io::write_all`, `file::read`/`file::write` via blocking pool, opaque `fd_t` wrapper with no implicit int conversion
- The gap is mostly about naming alignment, the `fd_t` wrapper type, and file I/O helpers — the hard I/O plumbing is done.

### 🎯T3.2 Channel-native HTTP/1.1 server works  [weight 1.6]
Gap: not started
No llhttp vendored, no `http::serve`. Depends on 🎯T3.1.

### 🎯T3.5 WebSocket support (server and client)  [weight 1.6]
Gap: not started
No wslay vendored. Depends on 🎯T3.2 for HTTP upgrade path.

### 🎯T3.6 HTTP client  [weight 1.0]
Gap: not started
No HTTP client implementation. Depends on 🎯T3.1.

### 🎯T13 Per-worker wake eliminates thundering herd  [weight 0.4]  (status only)
Status: not started
No changed files overlap.

### 🎯T14 Dynamic scoping improvements  [weight 0.5]  (parked)
Gap: parked
No change since last report. Revisit if profiling points here.

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

Work on: **🎯T3.1 Ergonomic I/O wrappers exist**
Reason: Despite 🎯T7.3/T7.4/T7.6 having slightly higher effective weight (1.7 vs 1.6), 🎯T3.1 is **close** rather than not-started — most of the I/O infrastructure already exists. Closing it is cheaper and unblocks the entire 🎯T3 subtree (HTTP/1.1, WebSocket, HTTP client — all weight 1.6). The example targets are independent leaves with no downstream impact. 🎯T3.1 is higher leverage per unit of effort.

## Suggested action

Audit the existing I/O surface against 🎯T3.1 acceptance criteria and close the gaps: (1) introduce an opaque `fd_t` type wrapping `socket_t` with no implicit int conversion, (2) change `byte_reader`/`byte_writer` to assert non-blocking rather than calling `set_nonblock`, (3) add `io::read_all`/`io::write_all` convenience functions, (4) add `file::read`/`file::write` via the blocking pool, (5) add reference docs and tests for new additions.

<!-- convergence-deps
evaluated: 2026-04-06T00:00:00Z
sha: b9983a0

🎯T7.3:
  gap: not started
  assessment: "No web crawler example in examples/."
  read:
    - examples/

🎯T7.4:
  gap: not started
  assessment: "No sensor fusion example in examples/."
  read:
    - examples/

🎯T7.6:
  gap: not started
  assessment: "No log aggregator example in examples/."
  read:
    - examples/

🎯T3.1:
  gap: close
  assessment: "socket_t, net::listen, net::dial, byte_reader, byte_writer, split_lines all exist. Missing fd_t opaque wrapper, read_all/write_all, file::read/write. byte_reader/byte_writer set_nonblock instead of asserting."
  read:
    - include/csp/io.h
    - include/csp/net.h
    - include/csp/part/io.h
    - include/csp/byte_reader.h

🎯T3.2:
  gap: not started
  assessment: "No llhttp vendored, no http::serve."
  read: []

🎯T3.5:
  gap: not started
  assessment: "No wslay vendored."
  read: []

🎯T3.6:
  gap: not started
  assessment: "No HTTP client implementation."
  read: []

🎯T13:
  gap: not started
  assessment: "unpark_one() still uses notify_all. No implementation progress."
  read: []

🎯T14:
  gap: parked
  assessment: "Parked. HAMT works, not a bottleneck."
  read: []

🎯T12:
  gap: achieved
  assessment: "All test names use hyphens."
  read: []

🎯T11:
  gap: achieved
  assessment: "All criteria met. PR #18 merged."
  read: []

🎯T15:
  gap: achieved
  assessment: "PicoTLS vendored, mbedTLS removed, all tests pass."
  read: []

🎯T7:
  gap: achieved
  assessment: "3/6 sub-targets complete. Acceptance threshold met."
  read: []

🎯T2:
  gap: achieved
  assessment: "diff, frame, reorder, race implemented."
  read: []

🎯T4:
  gap: achieved
  assessment: "closer<EP> and main() error message both done."
  read: []

🎯T5:
  gap: achieved
  assessment: "3 TLA+ spec pairs written."
  read: []

🎯T8:
  gap: achieved
  assessment: "Signal handling audited, no violations."
  read: []
-->
