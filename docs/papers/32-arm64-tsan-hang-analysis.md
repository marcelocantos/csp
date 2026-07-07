# Paper 32 — Linux arm64 dist-TSan CI hang: analytic scan

🎯 Target: T29
Status: **open** — scan complete, candidates ranked, no confirmed root cause.
Filed: 2026-07-05.

## Symptom

`Linux arm64 TSan` CI job hung 30m17s on PR #82 (a pure-data change) and was
cancelled by `timeout-minutes: 30`. The hang was in `Build and test (dist,
thread)` — the `build/thread-dist-notls` suite. Normal envelope: 2–3 min.
x86_64 TSan passed at 3 min in the same run. A re-run passed. One occurrence
observed; nothing captured (a timeout-cancellation is not a `failure()`, so
the GDB step never fired — and its hardcoded binary path is stale anyway).

## Method

Whole-protocol read of the scheduler sleep/wake surface, with an explicit
happens-before argument for each transition, before touching any fix. The
per-protocol TLA+ inventory (PerWorkerWake, DrainSuspended, StealWork,
WorkerParking, QuiescenceScope/Deadline, MainLoopExit, SchedulerTermination,
each with a `_Bug` mutation twin) verifies these protocols **under sequential
consistency, in isolation**. A green spec bounds where the bug can live: either
below the specs' abstraction (fences, TSan runtime interaction, timing regime)
or between specs (composite interactions no single spec models).

## Actors audited and found sound

Each of these was read end-to-end; the happens-before chain that closes the
obvious lost-wakeup window is noted.

1. **Worker park/unpark** (`runtime.cpp:206-279`, `unpark_one` 158-195).
   Worker: `parked=true` (release) → notify `park_cv` under `park_mu` →
   `has_work()` recheck (takes `global_mu`) → `note.sleep()`. Pusher:
   `push_to_global` under `global_mu` → `unpark_one`. The two `global_mu`
   critical sections totally order push vs. recheck: worker either sees the
   work, or the pusher sees `parked=true` (via `global_mu` release/acquire
   chain) and wakes/flags the Note. Sound.
2. **Note state machine** (`note.h`). AWAKE/SLEEPING/FLAGGED with
   wake-before-sleep → FLAGGED, consumed on next `sleep()`. The timed
   `sleep_for` race (wake vs. timeout) is safe because every caller rechecks
   `has_work` after waking, and a surplus worker cannot exit with work in
   the global queue (its exit gate takes `global_mu`). Sound.
3. **Channel suspend/wake** (`channel.cc:437-459`, `csp.cc:95-116, 183-209,
   287-298`). `suspending_=true` is stored **before** `unlock_all`, and a
   waker can only discover the waiter under the channel locks the waiter
   released — so by the time the waker's `schedule()` runs, `suspending_`
   is visible; it defers via `wake_pending_`. `drain_suspended` clears
   `suspending_` and drains `wake_pending_` under `global_mu`, mutually
   exclusive with `schedule()`'s check. The `run(detach)` fast-path
   `wake_pending_` exchange honors the wake on both orderings. Sound.
4. **`schedule()`'s `next_` early return** — only reachable when
   `suspending_==false`; a channel waiter mid-suspend always has
   `suspending_==true`, so the "running imp still linked in the busy ring"
   state is never mistaken for "queued". Sound.
5. **quiescence_scope** (`csp.h:239-280`, `csp.cc:211-219, 329-376`).
   `leave()` notifies under `mu_` on the 1→0 transition; `wait()` evaluates
   the predicate under `mu_` — the mutex handshake closes the
   notify-before-block race. enter-at-schedule-time (`make_runnable`'s
   `qs_sleeping_` exchange) balances enter/leave across the leave→enter
   inversion when a waker races `do_switch`. Sound.
6. **TSan fiber wiring** (`csp.cc:132-134, 235-237, 572-574`,
   `csp_globals.cpp:79-81`). Fiber created before the warmup switch; per-P
   main imps bound via `__tsan_get_current_fiber()`; destroy on exit. Sound.
7. **TLS-caching class** (`csp_globals.cpp`, dist mirror). All five accessors
   are `CSP_TLS_NOINLINE`; `csp_globals.cpp` remains a separate TU in dist,
   so cross-TU calls defeat address caching. No LTO in CI. Closed (🎯T16).
8. **park_cv notify discipline**. All *terminal* notifies (worker park,
   `destroy_imp` completion, shutdown) hold `park_mu` before notifying.
   `unpark_one`'s unfenced `notify_all` can be missed by a waiter between
   predicate-eval and block, but every such waiter is re-notified by a later
   fenced event (a park or a completion). Benign.

## Ranked candidates

### C1 — watchdog `add_processor` congestion cascade under TSan (primary hypothesis)

`watchdog_loop` (`runtime.cpp:352-381`) treats a 10ms-frozen heartbeat on an
unparked worker as a stall and calls `add_processor` — every 10ms tick, up to
`max_procs_ = max(procs, 4×hw)` (16 on a 4-core runner). The heartbeat
increments once per `worker_loop` iteration, so any imp that runs >10ms
between yields freezes it. TSan slows everything 10–20×, pushing many
ordinary test sections past 10ms — and `mn.test.cc:647-720` *deliberately*
busy-spins workers for 200ms–2000ms (`MN---Watchdog-rescues-stalled-P`,
`MN---Watchdog-rescues-timers-from-stalled-P`) with `set_maxprocs(2)`.
The regime: stalled Ps → processor churn to the 16-cap → 16 TSan-instrumented
threads contending TSan's internal shadow/clock locks on 4 cores → each
iteration slower → more heartbeat misses → sustained thrash. Surplus Ps park
with 5s wind-down sleeps and get re-added, so the churn is self-renewing
while any stall persists.

This is a **congestion collapse, not a deadlock** — which fits every
observable: 30 min vs 2–3 min (slow-crawl past the cap, not zero progress),
one-off occurrence (needs the marginal regime to tip), re-run green, trigger
on a pure-data PR (same code, different scheduling dice), and arm64
specificity (different runner perf profile and TSan overhead than x86_64).

Discriminator: the T29 watchdog's `thread apply all bt` on SIGQUIT. C1 shows
~16 `csp-N` threads, most runnable/spinning; a true lost-wakeup deadlock
shows 2–3 threads all blocked in futex/cv waits.

### C2 — dual time-advancer (fake_clock::run loop × main_loop hook)

`fake_clock::sleep_until` lazily registers the main_loop quiescence hook
(`clock.cc:56-68`) even when a `fake_clock::run()` driver is already
advancing time via `qs_.wait()` (`clock.cc:113-119`). Two concurrent
`advance_to_next()` callers can double-advance, and a racing "no timers"
verdict can make `run()` exit its loop early. Also: when the scope is
quiescent with `pending_` empty but the hook registered, `main_loop`'s
predicate stays true and it busy-spins (paper 22's shape, with a hook).
No concrete hang trace constructed; consequences look test-dependent and
mostly self-healing (the hook backstops an early-exited `run()`). Worth a
composite TLA model (see oracle plan), not a blind fix.

### C3 — `fake_clock::advance()` writes `current_` unlocked

`clock.cc:90-93`: `current_ += d` outside `mu_`, racing `advance_to_next()`'s
locked write and `now()`'s unlocked read (the latter suppressed as benign).
A write-write race between `advance()` and the hook is possible in principle
under C2's dual-advancer condition; a torn/lost advance strands a sleeper
whose deadline is never reached. TSan has never reported the write-write
pair, so the dynamic condition is at least rare. Cheap, defensible fix
(take `mu_` in `advance()`), but attribution-neutral: fix it and note it,
don't credit it with the hang absent evidence.

### C4 — test-side real-time waits starved under TSan

`mn.test.cc:663-710` busy-waits (deliberate, see C1); `io.test.cc:908`
sleeps; `interleave.test.cc:62-102` busy-waits 50–100ms. These *fail* loudly
rather than hang when deadlines slip — except as C1 fuel. Subsumed by C1.

### C5 — a genuinely narrow protocol hole below/between the specs

The honest residue. Every audited transition is sound under mutex-chained
happens-before, but the audit is a human read, and the specs don't compose
(no model covers worker parking × qs scope × hook × dual advancers × watchdog
together). If the watchdog's first capture shows a futex-wait deadlock
instead of C1's thread storm, this becomes primary and the composite spec
becomes the diagnostic tool.

## Hypothesis (named, per convention)

The hang is C1: watchdog-driven processor-pool thrash under TSan slowdown on
the 4-core arm64 runner, tipping the mn watchdog stress tests (or a similarly
slow section) into a congestion regime that exceeds the 30-min job cap
without ever deadlocking.

## Invariant believed violated

*The dist-TSan suite makes forward progress: some test retires in every
bounded window.* Today CI cannot observe violations (a hang is a cancel, not
a failure). The watchdog converts the invariant into a checkable property:
`timeout -s QUIT` → core → `thread apply all bt` → the next occurrence is
evidence, not folklore. (Local validation showed `-s ABRT` doesn't reach a
core: doctest installs its own SIGABRT handler, which prints a crash summary
and exits gracefully instead of coring — so the watchdog signals SIGQUIT,
which doctest does not trap.)

## Oracle plan (oracle-first)

| Artifact | Class conversion | Status |
|---|---|---|
| CI watchdog (`TEST_TIMEOUT` + find-based GDB step) | class 4 → observable evidence per occurrence | designed (T29 context), ready to implement |
| Local repro loop: `make docker-test-arm64 SANITIZE=thread` × N on Apple-silicon Docker, wall-time distribution + `num_procs_` high-water | class 4 → class 1 (statistical) | not built |
| Composite TLA spec (parking × qs × hook × advancers) | C2/C5 → class 1 | not built; gate on watchdog evidence first |
| `advance()` lock fix | removes C3 from the suspect set | trivial |

Iteration runs against the repro loop and TLC — never against CI re-runs
(~1 bit/day). The 20-run CI window in T29's acceptance is the final
accept/reject gate, not the iteration signal.

## Next steps

1. Ship the CI watchdog + stale-GDB-path fix (T29's scoped PR; this paper
   rides along). Purely observability — no behaviour change to the runtime.
2. Build the local repro loop; record a baseline distribution; check for a
   heavy tail and `num_procs_` saturation correlating with slow runs (C1).
3. If C1 confirmed: candidate mitigations to evaluate *against the repro
   loop* — scale the watchdog interval under `CSP_SANITIZED`-style gate,
   cap add_processor churn rate, or tick heartbeats at suspend checkpoints
   rather than loop iterations. Pick by evidence, not plausibility.
4. If the first capture shows a futex-wait deadlock instead: promote C5,
   build the composite spec, model the captured stack's exact actor set.

---

# Addendum (2026-07-07): second hang event, mechanism findings, and the work-gate fix

## Repro-loop findings (Apple-silicon Docker, arm64 dist-TSan, 4-CPU pin)

14 measured runs of the CI-shaped workload (`make test-dist SANITIZE=thread`,
ubuntu:24.04 arm64 container, `CSP_PROC_STATS=1` high-water instrumentation):

- Wall times tight (43–50 s, no tail — a 16-core M4 Max absorbs the storm).
- **`CSP_PROC_HIGH_WATER=64` in every run** — the processor pool saturates
  at `max_procs_ = 4×hardware_concurrency` (hw=16 in-container; `--cpuset`
  does not constrain `std::thread::hardware_concurrency`, which is
  sysconf-based). TSan thread numbering reached **T64075** in one report:
  ~64k worker threads created in a single suite run. C1's churn mechanism
  is real and fires *unconditionally* under TSan, not rarely.
- One run exited rc=2 with an unsuppressed TSan report — which turned out
  to be the *known* `fake_clock::now()` race (C3's read side). Reusable
  gotcha: **TSan suppressions match symbolized frames; without
  `llvm-symbolizer` in the environment every frame is `<null>` and the
  suppression file silently stops matching.** The docker image lacked
  llvm-18 (and also `libclang-rt-18-dev` — `make docker-test-* SANITIZE=…`
  had never worked on a clean image); both are now in the image recipe.

## Second hang event: macOS arm64, no sanitizer (2026-07-07)

CI run 28858231742 (`workflow_dispatch`, master): job `test (macOS arm64)`
cancelled at 30m19s. Step timeline: source suite 757/757 green; dist
CSP_TLS=1 suite 711/711 green in ~40 s; dist **CSP_TLS=0** suite printed the
doctest banner at 10:12:52 and then **nothing for 24.5 minutes** until the
cancel; runner cleanup terminated an orphaned `csp_tests`. No diagnostics —
the PR #91 watchdog only guarded the Linux arm64 TSan row.

**Common-factor analysis across both hang events:**

| | 2026-06-30 | 2026-07-07 |
|---|---|---|
| OS/arch | Linux arm64 | macOS arm64 |
| Sanitizer | TSan | none |
| Suite | dist `CSP_TLS=0` | dist `CSP_TLS=0` |
| Sibling suites in same run | passed | source + dist TLS=1 passed |

The hang is **not TSan-specific**: TSan was an amplifier, not the cause.
C1 is demoted from root cause to (real, fixed) aggravator; **C5 is promoted
to primary** — a rare, platform-agnostic hang, and the shared configuration
is the **dist CSP_TLS=0 suite**. That collapses the search space: the notls
dist build differs by amalgamated-TU codegen and by the `#ifdef CSP_TLS`-off
paths; the suite subset differs only in TLS-dependent tests self-skipping.

## The watchdog calibration fix — three iterations against the oracle

The suite runs with `CSP_MAXPROCS=1` by default (test/main.cc; → 2-proc
runtimes, with explicit `set_maxprocs(2/4)` in the mn tests), so the
process-global high-water of 64 means the watchdog inflated a **2–4 proc
pool to the 64-slot cap — a 16–32× over-provision**. The native (no
sanitizer) macOS hang-hunt runs show high-water 64 as well: the storm is
not TSan-specific; TSan merely guarantees it.

Two plausible fixes failed against the oracle before the real one landed
(each validated on 5 docker arm64 dist-TSan runs with `CSP_PROC_STATS`):

1. **Work gate** — only rescue a stalled P when a stealable MT exists in
   its ring (mirroring `steal_work`'s candidate test) or global work is
   pending. High-water: still 64. Under a live suite, work almost always
   exists; the gate rarely bites.
2. **Reuse before add** — wake a parked worker instead of creating a new
   one. High-water: still 64. During the storm nobody is parked: every P
   is busy running slow-but-progressing slices.
3. **Stall hysteresis (decisive)** — the actual defect is the stall
   discriminator: at one 10 ms tick, a healthy worker whose slice runs
   15–50 ms (routine under TSan, common enough natively) is
   indistinguishable from a blocked one, and the watchdog's job is to
   rescue *blocked* workers, not slow ones. Requiring 10 consecutive
   no-progress ticks (100 ms) before declaring a stall dropped high-water
   **64 → 4** (zero watchdog adds across the whole suite; the 4 is the
   largest explicit `set_maxprocs`), 681/681 green, wall time unchanged,
   on all 5 validation runs.

All three changes ship together (the gate and reuse-first are correct
hygiene even though each alone was insufficient), plus
`fake_clock::advance()` now writes `current_` under `mu_` (C3's
unsuppressed write-write side). The mn watchdog stress tests — which
*require* rescue (200 ms/2000 ms deliberate stalls with queued work) —
pass unchanged: 100 ms detection latency is far inside their timescales.

## Flaky-test harvest from the hang-hunt

The native dist-notls loop (M4, ~22 s/run) is a far harsher interleaving
oracle than CI runners, and surfaced five distinct latent test bugs in its
first 56 runs (~13% aggregate failure rate) — each a threat to the T29
criterion-1 window's stability, all test-side, all fixed in this PR:

| Test | Defect |
|---|---|
| `Channel---AltDead` | `yield()` assumed the vulture fires before an 11th request registers; fair `alt` may pick the request. Now waits for server death via endpoint-death read. |
| `Mux---input-death-continues` | Asserted cross-channel arrival order between concurrent writers; mux promises none. Now order-agnostic. |
| `Thread---SpawnSpawn` | `int result` incremented by 25 concurrent imps across OS threads — lost increments. Now atomic. |
| `cancel-during-I/O` | Closed the pipe's write end before the cancel propagated; EOF could complete the read without a `canceled` throw. Now waits for child exit before closing. |
| `Thread---Yield` | Two imps appended to one `std::string` unsynchronized — UB. Now mutex-serialized. |

## The hang, reproduced locally

Hang-hunt run 56 (`CSP_MAXPROCS=4`, native macOS dist-notls): doctest
banner, then silence past the 180 s deadline — the exact CI signature, at
~1/28 per maxprocs4 run. No stack captured (macOS `/cores` needs sudo);
hunt v2 targets the same specimen binary with `sample`-based all-thread
capture (two samples 2 s apart: identical ⇒ deadlock, changing ⇒
livelock) before the kill.

## Revised hunt for C5

The configuration is locally runnable natively — no docker, no sanitizer:
`build/normal-dist-notls/csp_tests` on the M4, ~10 s per iteration, looped
under `gtimeout -s QUIT` with `/cores` capture and two arms (unconstrained,
`CSP_MAXPROCS=4` mimicking the 3–4 vCPU runners). A captured core's
`bt all` is the discriminator the June event never produced: a futex/cv
wait cluster names the deadlock site (C5); the watchdog extension shipped
alongside this addendum (TEST_TIMEOUT on every dist-capable CI row, macOS
core capture + lldb step) does the same for the next CI occurrence.
