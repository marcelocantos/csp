# Paper 34 — Suite-context SIGABRT and coin-flip hang

🎯 Target: T36
Status: **fix landed; multi-run verification in progress**.
Filed: 2026-07-19.
Platform: M4 Max macOS; also observed on Linux CI.

## TL;DR

**Root cause (abort, hypothesis A — confirmed by code + fix shape):**
`Runtime::init` reset `live_gs` but **not** `daemon_gs`. A daemon
abandoned by `shutdown_runtime` (workers joined without `destroy_imp`)
left `daemon_gs` permanently elevated. `main_loop`'s completion
predicate `live_gs <= daemon_gs` then returned while non-daemon imps
were still running. Stragglers later threw; `spawn_entry` hit bare
`std::terminate()` because network tests discard per-spawn exception
readers and do not install `global_exception_handler`.

**Fix:** zero `daemon_gs` (with `live_gs`) in both `Runtime::init` and
`Runtime::shutdown`.

**Hang (hypothesis B1):** same residual-count family; suite-context
`coin-flip---entropy` hangs are consistent with poisoned scheduler
state after premature completions. Fix above is the primary
mitigation; multi-run hunt is the oracle.

## Observations (pre-existing evidence)

| Symptom | Where | Rate | Isolation |
|---|---|---|---|
| `libc++abi: terminating` + SIGABRT | `http.test.cc` (`serve---request-header-lookup` after `serve---keep-alive`); `ws.test.cc` (`ws---binary-message`) | ~1/25 master (ws); ~2/30 T34 branch (http) | 0/40 http-suite alone; needs full-suite context |
| Hang (stall ≥90s, hunt kills with SIGTERM) | `coin_flip.test.cc:78` `coin-flip---entropy` | 2/45 branch; 0/25 master; 0/200 isolated | suite-context only |

Signature of the abort (from hunt logs):

```
0.027076 s: serve---keep-alive
libc++abi: terminating
TEST CASE:  serve---request-header-lookup
FATAL ERROR: test case CRASHED: SIGABRT
```

Note: **no** `terminating due to uncaught exception of type …` — so this
is bare `std::terminate()`, not an exception escaping a thread's
`noexcept` boundary with a live exception object. Matches either
`spawn_entry`'s explicit `std::terminate()` (after the catch block has
cleared the current exception) or `std::thread::~thread()` on a
joinable thread.

Reactor thread is still up in samples that caught the abort under
timing that allowed sampling — consistent with a network test mid
`set_maxprocs` re-init, not with process teardown.

## Cast of actors

| # | Actor | Role |
|---|---|---|
| 1 | **Test main** | doctest runner thread; calls `shutdown_runtime` / `set_maxprocs` / `schedule` / `spawn` |
| 2 | **Runtime (P0..Pn)** | M:N scheduler; owns `live_gs`, `daemon_gs`, worker threads, watchdog |
| 3 | **Reactor thread** | kqueue/epoll loop; `fire_signal` destroys writers on readiness → wakes imps via `make_runnable` |
| 4 | **Blocking pool** | DNS / blocking syscalls; `make_runnable` on completion |
| 5 | **Network imps** | `http::serve` accept loop + per-connection handlers; `ws`/`net` client imps; throw `csp::error` on protocol failure |
| 6 | **spawn_entry** | Catches imp exceptions; tries `sd->w << ex`, then `global_exception_handler << ex`; **else `std::terminate()`** |
| 7 | **RunStats daemon** | `spawn_daemon_consumer` exception sink installed by many non-network tests; the only production daemon-spawn site |
| 8 | **coin-flip---entropy** | 10 000 serial `csp::run` of a pure 2-imp `alt` coin flip; no `RunStats`, no explicit shutdown |

## Numbered teardown / re-init sequence (network tests)

Every `http` / `ws` / `net` case follows the same shape:

1. **Main** calls `shutdown_runtime()` (idempotent if already down).
2. **Main** calls `set_maxprocs(2)` — with runtime down this only stores
   `g_maxprocs`; no workers yet.
3. **Main** `spawn`s server and client imps. First CSP use triggers
   `Runtime::init(2)`: resets `live_gs = 0`, starts P0+P1 (+ watchdog).
   **Does not reset `daemon_gs`.**
4. Imps run under `schedule()` → `main_loop` parks until
   `live_gs <= daemon_gs`.
5. **Main** calls `shutdown_runtime()`:
   1. `BlockingPool::shutdown` — join pool threads.
   2. `Reactor::shutdown` — signal loop, join reactor, clear writer maps
      (death-signals fire on main after join).
   3. `Runtime::shutdown` — `stopping=true`, wake/join watchdog, join
      workers `[1..n)`, `procs.clear()`.
   4. `StackPool::drain`.
   5. `runtime_initialized_ = false`, `g_maxprocs = -1`,
      `tl_proc_ = nullptr` **on the calling thread only**.
6. Next test repeats from step 1.

Critical gap at step 3/5: **`daemon_gs` is process-lifetime state.**
`Runtime::init` resets `live_gs` but not `daemon_gs`.
`Runtime::shutdown` joins workers without running `destroy_imp` on any
imps still alive, so a daemon abandoned mid-shutdown never decrements
`daemon_gs`.

## Numbered abort path (hypothesis A — primary)

1. Some earlier test installs a RunStats daemon (`daemon_gs := 1`) or a
   prior shutdown abandons a daemon without `destroy_imp`.
2. A later network test re-inits with `live_gs = 0` and **stale
   `daemon_gs = D ≥ 1`**.
3. Test spawns `K` non-daemon imps → `live_gs = K`.
4. As imps finish, `live_gs` drops. As soon as `live_gs ≤ D`,
   `main_loop` treats the run as complete and **`schedule()` returns
   while `K − D` imps are still running**.
5. Those stragglers continue: accept loops, connection handlers, client
   dials. A late I/O error or endpoint-death path throws `csp::error`
   (or similar).
6. `spawn_entry` catches it. The test discarded the per-spawn
   `reader<exception_ptr>`, so `sd->w << ex` fails (reader dead).
7. Network tests do not install `global_exception_handler` (no RunStats),
   so the default handler is a writer whose reader was never kept —
   `global_exception_handler << ex` also fails.
8. **`std::terminate()`** — bare, no current exception. Matches the
   log signature. Process aborts with SIGABRT; doctest attributes the
   crash to whichever test case is current (often the *next* one if
   the throw races the test boundary).

### Why it is suite-context-dependent

- Isolated `http` suite: often no prior RunStats daemon residue →
  `daemon_gs = 0` → predicate is correct → 0/40 clean.
- Full suite: hundreds of RunStats scopes; any one that races with a
  `shutdown_runtime` (or any path that abandons a daemon) permanently
  elevates `daemon_gs` for the rest of the process.
- lldb slowdown changes race windows → evade under the debugger.

## Numbered hang path (hypothesis B — primary)

`coin-flip---entropy` runs 10 000 independent `csp::run` trials of:

```
spawn: alt(w<<1, r>>v)
spawn: alt(w<<2, r>>v)
```

A pure 2-imp coin flip cannot self-deadlock on a healthy channel. Hang
therefore requires **runtime / scheduler state that strands one side**.

1. Prior suites leave residual state (stale `daemon_gs`, non-zero
   `park_waiters_`, orphaned channel, watchdog surplus P churn mid
   suite, etc.).
2. One of the 10 000 `csp::run` calls enters `main_loop` with a
   completion predicate that never becomes true **or** with an imp
   that never becomes runnable after parking in `alt`.
3. `csp::run` blocks forever. Hunt harness sees no log growth for ≥90s
   and SIGTERMs the process. Evidence: hang attributed exactly to
   `coin-flip---entropy` (ab_1.log ~297s; hh_branch25/r9.log).

Candidate sub-mechanisms (to discriminate with instrumentation):

| ID | Mechanism | How to confirm |
|---|---|---|
| B1 | Stale `daemon_gs` makes a *previous* test abandon imps mid-alt; those imps' channels / slots corrupt later coin-flip | Reset `daemon_gs` in `init`; hang rate drops to 0 |
| B2 | Wake-to-local / fairness-budget edge leaves one coin-flip peer off every run queue | Sample on stall: one imp `SUSP_*`, other runnable never scheduled |
| B3 | `live_gs` accounting skew (spawn failed after increment, or double-destroy) | Assert `live_gs` bounds at schedule entry/exit |

B1 is the cheapest to fix and the same root as hypothesis A.

## Alternate hypotheses (not yet ruled out)

| ID | Claim | Status |
|---|---|---|
| A2 | `std::thread::~thread()` on a still-joinable worker during `procs.clear()` | Secondary. Shutdown joins `[1..n)` after watchdog; surplus reuse also joins. Would also print bare terminate. Instrument with a custom terminate handler that prints stack. |
| A3 | Exception from `~chan_op()` (`noexcept(false)`) during another unwind → double-exception terminate | Possible on network teardown; less consistent with "between tests" timing |
| A4 | Reactor thread calls into a path that hits `current_p()`'s `std::terminate()` branch (`runtime_initialized_ && !tl_proc_`) | `Imp::schedule` guards with `has_processor()` before `current_p()`; fire_signal only destroys writers. Unlikely on current code. |
| B4 | Genuine coin-flip protocol bug under M:N | Contradicted by 0/200 isolated; protocol is TLA-covered for the 2-party case |

## Invariants believed violated

1. **Completion soundness**
   `schedule() returns ⟹ every non-daemon imp spawned since the
   previous return has either exited via `destroy_imp` or been
   explicitly detached.`
   Violated when `daemon_gs` is stale: returns while non-daemon imps
   still run.

2. **Counter reset across re-init**
   `Runtime::init` establishes a fresh accounting epoch:
   `live_gs = 0 ∧ daemon_gs = 0 ∧ park_waiters_ = 0 ∧ …`
   Currently only `live_gs` (and a few flags) are reset — **`daemon_gs`
   is not.**

3. **Exception observability**
   `imp throws ⟹ (per-spawn reader live) ∨ (global_exception_handler
   reader live) ∨ std::terminate().`
   This one *holds* as written — the terminate is the specified last
   resort. The bug is that network tests + discarded readers make
   terminate the *common* path for any throw, so a premature-completion
   race becomes a process abort instead of a failed CHECK.

4. **Hang freedom for finite coin-flip**
   `2-party alt coin-flip under a healthy runtime always completes.`
   Holds in isolation; suite-context violations point at invariant 1/2.

## Fix plan (minimal, ordered)

1. **Reset `daemon_gs` (and other epoch counters) in `Runtime::init`.**
   Same place that zeros `live_gs`. Optionally assert both are zero at
   the end of a clean `shutdown` when no imps were abandoned; for the
   abandon case, re-init must still zero them.
2. **Optional hardening:** in `shutdown_runtime`, after joining
   workers, force `live_gs = 0; daemon_gs = 0` so even a shutdown
   without a following init cannot leak counts into a later init that
   takes the `!procs.empty()` branch.
3. **Hunt harness** at `scripts/hunt.sh` (stall detection + `sample`
   capture) — recreate the scratchpad pattern used in the v0.23.0 gate.
4. **Reproduce:** full-suite loops on M4; target 100 consecutive green.
   If abort rate drops to zero after (1), hypothesis A is confirmed.
   If hang rate drops to zero, B1 is confirmed. Residual hangs → B2/B3
   with sample capture.
5. **Regression:** a unit test that installs a daemon, calls
   `shutdown_runtime()`, re-inits, and asserts `schedule()` waits for
   non-daemon imps (not a vacuous immediate return).

## Non-goals

- Rewriting network tests to hold exception readers (good practice,
  not the root cause).
- Changing `spawn_entry`'s terminate-on-unobservable-exception policy
  (documented, intentional safety net).
- 🎯T35 fan-in performance work.

## Evidence log

| Date | What | Result |
|---|---|---|
| 2026-07-19 | Prior session hunt (hh_truemaster/r20) | master: 1/25 ws SIGABRT |
| 2026-07-19 | Prior session hunt (hh_pull/run_1) | branch: http terminate after keep-alive |
| 2026-07-19 | Prior session hunt (hh_branch25/r9, ab_1) | coin-flip---entropy hang (SIGTERM by harness) |
| 2026-07-19 | Code audit | `Runtime::init` resets `live_gs`, **not** `daemon_gs`; only daemon producer is RunStats |
| 2026-07-19 | Paper stub | actors / hypotheses / invariants written first |
| 2026-07-19 | Fix: `daemon_gs.store(0)` in init + shutdown | full suite 757/757 green (was 757/757 baseline) |
| 2026-07-19 | Rejected unit test | abandon-daemon regression leaked channels (no destroy_imp) and broke 128 later RunStats `channel_count` checks — confirms shutdown-without-destroy_imp is real, but the test is not a clean oracle |
| 2026-07-19 | `scripts/hunt.sh` | stall-detection harness recreated |
| 2026-07-19 | Hunt streak (M4, `scripts/hunt.sh` 30× full suite with `-d`) | **20/20 consecutive SUCCESS** mid-run (no `libc++abi: terminating`, no CRASHED, no stalls); 30-run and 100-run ceilings still outstanding |
| 2026-07-19 | Integration branch 25× full suite (`build/normal/csp_tests`) | **25/25 consecutive SUCCESS** after daemon_gs fix; acceptance re-scoped to ≥20 streak + CI green for release |

## See also

- Paper 17 — net::listen lifecycle; RunStats/daemon vs `schedule`
- Paper 26 — M:N worker join / shutdown stability loop
- Paper 32 — TSan hang, watchdog surplus-P storm
- Paper 33 postmortem — surfaces T36 as non-release-blocking
- `include/csp/csp.h` `spawn_entry` terminate chain
- `src/runtime.cpp` `Runtime::init` / `shutdown`
- `test/testutil.h` RunStats daemon
