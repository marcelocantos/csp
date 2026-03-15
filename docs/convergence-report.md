# Convergence Report

**Generated**: 2026-03-15
**Branch**: master
**SHA**: fe73f81

## Standing invariants

- Tests: 636/636 passed (local macOS).
- CI: 9/11 on last PR (#13). Two pre-existing flakes: Linux arm64 TSan (cancel timing), macOS TSan (mbedTLS). Tracked by 🎯T9. Non-TSan jobs all green.

## Movement

- 🎯T7.1: (unchanged) — chat server committed (#11), shutdown crash remains
- 🎯T9: (new) — created to track TSan CI flakes
- 🎯T6: not started → close (Docker targets exist, documented in CLAUDE.md)
- All other targets: (unchanged)

## Gap report

### 🎯T7 Non-trivial example applications demonstrate CSP  [weight 1.7]
Gap: converging (0/6 sub-targets achieved, 1 close)

  [~] 🎯T7.1 Chat server example — close: committed and compiles, shutdown crash remains (TODO item)
  [ ] 🎯T7.2 ETL pipeline example — not started
  [ ] 🎯T7.3 Web crawler example — not started
  [ ] 🎯T7.4 Sensor fusion dashboard example — not started
  [ ] 🎯T7.5 Task scheduler example — not started
  [ ] 🎯T7.6 Log aggregator example — not started

### 🎯T6 Local Docker testing covers Linux scenarios  [weight 1.5]
Gap: close
`make docker-test`, `docker-test-arm64`, `docker-test-x86` all exist. Sanitizer support documented. Documented in CLAUDE.md. Only gap: README mention and verifying x86 cross-compilation works end-to-end.

### 🎯T4 API safety gaps are closed  [weight 1.2]
Gap: not started (status only)

  [ ] 🎯T4.1 `closer<EP>` enforces vulture-only endpoints — not started
  [ ] 🎯T4.2 main() can perform CSP operations — not started

### 🎯T5 Unmodeled concurrent decision points have TLA+ specs  [weight 1.0]  (status only)
Status: not started

### 🎯T8 Signal handling is audited for correctness  [weight 1.0]  (status only)
Status: not started

### 🎯T3 Runtime is production-ready for I/O workloads  [weight 0.6]  (status only)
Status: not started (0/4 sub-targets). Cost exceeds value at current estimates.

### 🎯T2 Tier D combinators are implemented  [weight 0.6]  (status only)
Status: not started

### 🎯T9 TSan is clean on all CI jobs  [weight 0.6]  (status only)
Status: not started. mbedTLS fiber annotation gap and cancel timing flake.

## Recommendation

Work on: **🎯T6 Local Docker testing covers Linux scenarios**
Reason: Highest weight (1.5) among close-to-achieved targets. Gap is minimal — Docker targets already exist and work. Closing this is ~30 minutes of verification and documentation, freeing bandwidth for 🎯T7 examples next.

## Suggested action

Run `make docker-test` locally to verify both ARM64 and x86_64 builds pass. If x86 cross-compilation doesn't work, fix it. Then update the README with Docker testing instructions and mark 🎯T6 achieved.
