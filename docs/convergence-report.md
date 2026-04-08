# Convergence Report

**Generated**: 2026-04-08
**Branch**: master
**SHA**: eff9850

## Standing invariants

- Tests: 668/668 passed.
- CI: **GREEN** (all 3 recent runs succeeded, including PR #21 merge).
- No open PRs.

## Movement

- 🎯T7.3 Web crawler example: not started -> **achieved** (v0.8.0, PR #20)
- 🎯T7.4 Sensor fusion dashboard example: not started -> **achieved** (v0.8.0, PR #20)
- 🎯T7.6 Log aggregator example: not started -> **achieved** (v0.8.0, PR #20)
- 🎯T7 Non-trivial example applications: achieved (3/6) -> **achieved (6/6)**
- 🎯T16 Dist-build test suite does not hang on CI: (new) -> **achieved** (PR #21)
- 🎯T3.1 Ergonomic I/O wrappers exist: close -> **achieved**
- 🎯T3.2: (unchanged — not started)
- 🎯T13, 🎯T14: (unchanged)

## Gap report

### 🎯T3.2 Channel-native HTTP/1.1 server works  [weight 2]
Gap: not started
No llhttp vendored, no `http::serve`. Gateway target — unblocks 🎯T3.5 (WebSocket), 🎯T3.6 (HTTP client), 🎯T3.7 (HTTP/2). All dependencies satisfied (🎯T3.1 achieved).

### 🎯T3.5 WebSocket support (server and client)  [weight 2]  **BLOCKED by 🎯T3.2**
Gap: not started
No wslay vendored. Cannot start until HTTP/1.1 server provides the upgrade path.

### 🎯T3.6 HTTP client  [weight 1]  **BLOCKED by 🎯T3.2**
Gap: not started
Reuses llhttp for response parsing. Blocked on HTTP/1.1 work establishing shared types.

### 🎯T3.7 HTTP/2 support  [weight 1]  **BLOCKED by 🎯T3.2**
Gap: not started
No nghttp2 vendored. Blocked on shared HTTP types from 🎯T3.2.

### 🎯T3.8 QUIC transport  [weight 0.4]
Gap: not started
No ngtcp2 vendored. Largest networking target (cost 13). Independent of HTTP/1.1 chain.

### 🎯T3.9 HTTP/3 over QUIC  [weight 1]  **BLOCKED by 🎯T3.7 + 🎯T3.8**
Gap: not started
Depends on both HTTP/2 types and QUIC transport.

### 🎯T3.3 High-density stack scaling  [weight 0.4]  (status only)
Status: not started
No changed files overlap.

### 🎯T3.4 Context-aware stack depth analysis  [weight 0.2]  (status only)
Status: not started
No changed files overlap.

### 🎯T3 Runtime is production-ready for I/O workloads  [weight 1]
Gap: converging (1/9 sub-targets achieved)

  [x] 🎯T3.1 Ergonomic I/O wrappers exist — achieved
  [ ] 🎯T3.2 Channel-native HTTP/1.1 server — not started (gateway)
  [ ] 🎯T3.5 WebSocket — not started (blocked by T3.2)
  [ ] 🎯T3.6 HTTP client — not started (blocked by T3.2)
  [ ] 🎯T3.7 HTTP/2 — not started (blocked by T3.2)
  [ ] 🎯T3.8 QUIC transport — not started
  [ ] 🎯T3.9 HTTP/3 — not started (blocked by T3.7 + T3.8)
  [ ] 🎯T3.3 Stack scaling — not started
  [ ] 🎯T3.4 Stack analysis — not started

### 🎯T13 Per-worker wake eliminates thundering herd  [weight 0.4]  (status only)
Status: not started
No changed files overlap.

### 🎯T14 Dynamic scoping improvements  [weight 0.5]  (parked)
Status: parked
No change since last report.

### Achieved targets (since last report)

- 🎯T3.1 Ergonomic I/O wrappers exist — achieved
- 🎯T7 Non-trivial example applications (6/6) — achieved
- 🎯T16 Dist-build test suite does not hang on CI — achieved

### Previously achieved (unchanged)

- 🎯T2 Tier D combinators — achieved
- 🎯T4 API safety gaps — achieved
- 🎯T5 TLA+ specs — achieved
- 🎯T8 Signal handling audit — achieved
- 🎯T11 Scheduler always M:N — achieved
- 🎯T12 Test names no spaces — achieved
- 🎯T15 TLS PicoTLS — achieved

## Recommendation

Work on: **🎯T3.2 Channel-native HTTP/1.1 server works**

Both the markdown WSJF analysis and bullseye agree: 🎯T3.2 is the highest-weight unblocked target (weight 2, value 8 / cost 5). It is also the critical gateway — completing it unblocks three downstream targets (🎯T3.5 WebSocket, 🎯T3.6 HTTP client, 🎯T3.7 HTTP/2), making it the highest-leverage next step by a wide margin. All its own dependencies are satisfied (🎯T3.1 achieved).

## Suggested action

Vendor llhttp into `vendor/` (it is a single-file, parser-only library from Node.js). Define the `http::request` and `http::response` types with channel-based body streaming. Implement `http::serve(port)` that returns a `reader` of per-connection endpoint bundles, with each connection spawning an imp that reads HTTP requests via llhttp and exposes them as `reader<http::request>` + `writer<http::response>`.

## Bullseye scorecard

**Ranking**:        +1
**Blocking**:       +2
**Data quality**:   -2
**Overall**:        +1
**Markdown rec**:   🎯T3.2 Channel-native HTTP/1.1 server works
**Bullseye rec**:   🎯T2.2 Channel-native HTTP/1.1 server works
**Notes**: Ranking +1: bullseye correctly placed T2.2 (=md T3.2) at the top with weight 2, ahead of all others. The markdown rank.py also had it at weight 1.6, but did not capture the dependency edges from Context fields, so it showed T3.5/T3.6 as equal peers rather than blocked — bullseye's explicit depends_on edges make the gateway role visible. Blocking +2: bullseye correctly identifies T2.3, T2.4, T2.5 as blocked by T2.2, and T2.7 as blocked by T2.5+T2.6. The markdown rank.py showed zero blocked targets because dependencies were only in prose Context fields. Data quality -2: this was a bootstrap run, so all discovered dates are today and IDs don't match the markdown T-numbers (bullseye T2.2 = markdown T3.2). The ID mismatch will cause ongoing confusion. Additionally, bullseye's MCP server silently overwrote targets.md with a lossy render (lost historical context, achieved dates, last-evaluated marker) — had to `git checkout` to restore it. Future runs should keep IDs aligned, and the render side-effect needs guarding. Overall +1: bullseye's blocking analysis is meaningfully better than markdown's for this repo; the ranking agrees; the main gap is the ID mapping.

<!-- convergence-deps
evaluated: 2026-04-08T00:00:00Z
sha: eff9850

🎯T3.2:
  gap: not started
  assessment: "No llhttp vendored, no http::serve. Gateway target unblocking T3.5, T3.6, T3.7."
  read:
    - include/csp/io.h
    - vendor/

🎯T3.5:
  gap: not started
  assessment: "No wslay vendored. Blocked by T3.2."
  read: []

🎯T3.6:
  gap: not started
  assessment: "No HTTP client. Blocked by T3.2."
  read: []

🎯T3.7:
  gap: not started
  assessment: "No nghttp2 vendored. Blocked by T3.2."
  read: []

🎯T3.8:
  gap: not started
  assessment: "No ngtcp2 vendored. Largest networking target."
  read: []

🎯T3.9:
  gap: not started
  assessment: "Blocked by T3.7 + T3.8."
  read: []

🎯T3.3:
  gap: not started
  assessment: "No progress."
  read: []

🎯T3.4:
  gap: not started
  assessment: "No progress."
  read: []

🎯T3.1:
  gap: achieved
  assessment: "fd_t opaque wrapper, byte_reader/byte_writer, read_all/write_all, lines, file::read/write all exist."
  read:
    - include/csp/io.h

🎯T13:
  gap: not started
  assessment: "unpark_one() still uses notify_all."
  read: []

🎯T14:
  gap: parked
  assessment: "Parked. HAMT works, not a bottleneck."
  read: []

bullseye:
  ranking: 1
  blocking: 2
  data_quality: -2
  overall: 1
  markdown_rec: T3.2
  bullseye_rec: T2.2
-->
