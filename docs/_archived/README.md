# Archived documents

Point-in-time investigations, dated audits, and design notes for work that has
since shipped. Kept for provenance — they describe the state of the project at
the moment they were written and are **not** maintained against current code.

For current documentation see the [guide](../guide/01-getting-started.md),
[reference](../reference/README.md), and [architecture](../architecture.md).

| Document | Written for | Status |
|---|---|---|
| [build-perf-2026-04-11.md](build-perf-2026-04-11.md) | Build-time audit, 2026-04-11 | Landed; numbers are historical |
| [ci-cleanup-strategy.md](ci-cleanup-strategy.md) | Getting the CI matrix green | Superseded by the current `.github/workflows/ci.yml` |
| [convergence-report.md](convergence-report.md) | Convergence snapshot, 2026-04-08 | Superseded by `bullseye.yaml` |
| [windows-port.md](windows-port.md) | Design for the Windows port | Port shipped; see `CLAUDE.md` → Gates for the current Windows flow |
| [overview.md](overview.md) | Original project overview | Superseded by the guide chapters |
| [audit-fable-2026-07.md](audit-fable-2026-07.md) | Model-assisted audit, 2026-07 | Point-in-time |

Live design rationale that is *not* archived: [`tls-caching-bug.md`](../tls-caching-bug.md)
(why `csp_globals.cpp` must be a separate TU) and
[`stack-analysis-future.md`](../stack-analysis-future.md) (open roadmap).
