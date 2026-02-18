# CSP Testing Strategy

## Overview

The current test suite (281 tests across 18 files) is strong on happy-path
convergence testing: spawn N things, run to completion, check the sum.
TSan and ASan builds catch memory ordering and use-after-free bugs
statistically. What's missing falls into two categories:

1. **Formal verification** of concurrent protocols — proving that no
   interleaving can violate safety invariants, rather than hoping volume
   tests hit the bad ones.

2. **Targeted code-level tests** that force specific protocol states and
   edge cases that volume tests reach only probabilistically.

These two approaches are complementary. TLA+ proves the *protocol design*
is correct; code-level tests verify the *implementation* matches the
protocol and handles cases the models abstract away (memory management,
error paths, boundary conditions).

---

## Part 1: TLA+ Formal Verification

### Completed

**DrainSuspended** — Models the `suspending_` / `wake_pending_` /
`global_mu` protocol between `drain_suspended()` and `schedule()`.
Verified: `NoLostWakeup` holds across all 17 reachable states. Buggy
variant (no mutex) demonstrates the TOCTOU violation in 8 steps.

### Proposed specs

#### 1. StealWork — work-stealing dual-lock protocol

**What to model**: The interaction between `steal_work` (thief thread),
`do_switch` / `run()` (victim thread), and `schedule()` / `take_from_global`
(any thread).

**Variables**:
- `local_queue[P]` — set of MTs on each P's local DLL
- `global_queue` — set of MTs on the global queue
- `running[P]` — which MT each P is currently executing
- `in_global[MT]` — per-MT flag
- `run_mu[P]` — per-P mutex
- `global_mu` — global mutex
- `pc_thief`, `pc_victim` — program counters

**Key actions**:
- `DoSwitch`: victim sets `running = g_self` under `run_mu`
- `LocalNext`: victim picks next MT from local queue, sets `running`
- `StealAcquireLocks`: thief acquires `victim.run_mu`, tries `global_mu`
- `StealTryLockFail`: thief fails `try_to_lock` on `global_mu`, backs off
- `StealDelink`: thief delinks candidate from victim's DLL
- `StealPushGlobal`: thief pushes to global queue (both locks held)
- `TakeFromGlobal`: any worker takes from global queue under `global_mu`,
  then calls `schedule_local` under own `run_mu`
- `Schedule`: waker pushes MT to global under `global_mu`

**Invariants**:
- `NoDoublePlacement`: every MT is on exactly one queue or is `running`
  (not on two queues, not on a queue while `running`)
- `NoLostMT`: no MT disappears (always on some queue or running or
  completed)
- `NoDanglingState`: if `in_global[mt]` is true, mt is in global_queue
- `RunningNotStealable`: `steal_work` never steals `running[victim]`

**Why this matters**: The dual-lock protocol (`victim.run_mu` then
`global_mu` via `try_to_lock`) exists to avoid deadlock with
`take_from_global` (which acquires `global_mu` then `run_mu`). The
`running` pointer prevents stealing the actively executing MT. These
invariants are argued informally in comments but never proven across all
interleavings.

**Model size**: ~3 participants (victim worker, thief worker, waker),
~15 actions, estimated ~500-5000 reachable states. Well within TLC's
capacity.

#### 2. ChannelLifecycle — endpoint release and waiter wakeup

**What to model**: The interaction between `release()` (closing an
endpoint), `prialt_begin_impl` (registering as a waiter then sleeping),
and `alt_end_impl` (completing a match).

**Variables**:
- `refcount[endpt]` — per-endpoint refcount (writer, reader)
- `alive` — channel alive counter (2 → 1 → 0 → delete)
- `waiters` — set of registered waiters
- `alt_state[MT]` — `IDLE` | `WAITING` | `CLAIMED`
- `on_queue[MT]` — whether MT is on a run queue
- `channel_live` — whether the channel object still exists
- `pc_closer`, `pc_waiter1`, `pc_waiter2` — program counters

**Key actions**:
- `RegisterWaiter`: waiter enters `prialt_begin_impl` phase 2, sets
  `alt_state = WAITING`, adds self to waiters list
- `WaiterSleep`: waiter context-switches out (under channel lock)
- `CloserRelease`: closer decrements refcount; if zero, iterates
  waiters doing CAS (`WAITING → CLAIMED`) and calling `schedule()`
- `CloserDecAlive`: closer decrements `alive`; if zero, deletes channel
- `WaiterWake`: waiter resumes, cleans up registration under lock
- `SecondCloserRelease`: second endpoint hits zero on another thread,
  decrements `alive`

**Invariants**:
- `NoUseAfterFree`: no action accesses channel state after
  `channel_live = false`
- `NoLostWaiter`: if a waiter was `WAITING` when the channel died, it
  must be scheduled (either via CAS in release or via direct match in
  prialt_begin_impl phase 1)
- `ExactlyOneDelete`: `alive` reaches 0 exactly once (the `fetch_sub`
  returns 1 on exactly one thread)
- `NoDoubleClaim`: at most one thread transitions a waiter's
  `alt_state` from `WAITING` to `CLAIMED`

**Why this matters**: `release()` iterates waiters and vultures, doing
CAS on `alt_state` from multiple potential closers. A second channel
in the same `alt` could have its closer race with the first. The
`alive_` two-phase delete (writer side and reader side decrement
independently) is subtle. This protocol has never been formally verified.

**Model size**: ~3-4 participants (2 closers, 1-2 waiters), ~12 actions,
estimated ~200-2000 reachable states.

#### 3. WorkerParking — park/unpark/shutdown protocol

**What to model**: The interaction between `worker_loop` (parking on
`park_cv`), `unpark_one` (waking workers), and `shutdown` (stopping all
workers).

**Variables**:
- `parked[P]` — whether each worker is parked
- `stopping` — global shutdown flag
- `has_work[P]` — whether work is available
- `park_mu` — mutex for parking
- `pc_worker[P]`, `pc_shutdowner` — program counters

**Key actions**:
- `WorkerCheckWork`: worker checks `has_work`, finds none
- `WorkerPark`: worker sets `parked = true`, enters `park_cv.wait`
- `WorkerWake`: worker wakes (predicate true), sets `parked = false`
- `UnparkOne`: another thread calls `park_cv.notify_all()`
- `ShutdownSetFlag`: shutdown sets `stopping = true`
- `ShutdownAcquireMu`: shutdown acquires `park_mu` (empty critical
  section) to synchronize with workers entering `wait()`
- `ShutdownNotify`: shutdown calls `park_cv.notify_all()`
- `WorkerExit`: worker sees `stopping = true`, exits

**Invariants**:
- `NoLostShutdown`: after shutdown completes (flag set + notify), every
  worker eventually exits (no worker stuck in `wait()`)
- `NoSpuriousExit`: workers only exit when `stopping = true` or surplus
  wind-down

**Why this matters**: The shutdown CV race (bug #8 in the commit history)
was a real bug. The fix — acquiring `park_mu` before `notify_all` —
ensures workers that have checked the predicate but haven't entered
`wait()` yet will see the notification. A TLA+ model would prove this
fix is complete.

**Model size**: ~3 participants (2 workers, 1 shutdowner), ~10 actions,
estimated ~100-500 reachable states.

#### 4. AltStateCAS — multi-waker alt claim protocol

**What to model**: Multiple threads racing to claim a waiter registered
on multiple channels.

**Variables**:
- `alt_state` — `IDLE` | `WAITING` | `CLAIMED`
- `signal` — which channel won
- `on_queue` — whether the waiter is scheduled
- `pc_waker[i]` — per-waker program counters

**Key actions**:
- `WaiterRegister`: waiter sets `alt_state = WAITING` on N channels
- `WakerCAS`: waker i attempts CAS `WAITING → CLAIMED`
- `WakerSetSignal`: successful waker sets `signal = i`
- `WakerSchedule`: successful waker calls `schedule()`
- `LoserSkip`: failed CAS, waker moves on

**Invariants**:
- `ExactlyOneClaim`: at most one waker succeeds in the CAS
- `SignalConsistency`: the `signal` value corresponds to the winning waker
- `WaiterWakes`: the waiter ends up on a run queue

**Why this matters**: When a waiter is registered on multiple channels and
multiple peers arrive simultaneously, only one should win the CAS. This
is the core correctness property of the alt mechanism. The current test
(`AltFairness`) checks statistical distribution but doesn't verify the
mutual exclusion property across all interleavings.

**Model size**: ~3-4 participants (1 waiter, 2-3 wakers), ~8 actions,
estimated ~50-200 reachable states.

### Model-to-code correspondence

Each spec should follow the pattern established in `DrainSuspended.tla`:

1. **Action-to-code annotations**: every TLA+ action documents the
   source file, function, and line range it models.

2. **Atomicity justification**: every action that groups multiple
   operations explains why — same lock held, single atomic instruction,
   or state invisible to other threads before unlock.

3. **Buggy variants**: each spec should have a `_Bug.tla` companion that
   removes a key synchronization mechanism and demonstrates the
   invariant violation. This serves two purposes: (a) validates the
   invariant actually catches bugs, (b) documents exactly what the
   synchronization prevents.

4. **Review protocol**: when the C++ code changes (lock restructuring,
   new atomics, protocol changes), the corresponding TLA+ spec must be
   updated and re-verified. The `make check` target ensures this happens
   automatically for the correctness check, but spec-to-code review is
   manual.

### Build integration

Already done: `make check` runs TLC on all `formal/*.tla` specs
(excluding `_Bug` variants and `_TTrace` artifacts). Specs are checked
in CI alongside `make`, `make SANITIZE=thread`, and
`make SANITIZE=address,undefined`.

---

## Part 2: Targeted Code-Level Tests

### A. Protocol-level tests (`test/protocol.test.cc`)

These tests force specific protocol states that volume tests only reach
probabilistically.

#### A1. steal_work contention

Saturate one P's local queue, starve the other. Force a steal attempt
while the victim is mid-`do_switch`. Verify no MT is lost or duplicated
(total count matches). Run under TSan.

**Exercises**: `steal_work` dual-lock, `running` pointer skip, `try_to_lock`
backoff.

**Complements TLA+**: StealWork spec proves the protocol design; this test
verifies the C++ implementation under real contention.

#### A2. Concurrent channel close + alt sleep

N MTs in `alt` on the same channel. Close the channel from another MT on
a different P. Verify all sleeping MTs wake with correct dead-channel
(complemented) result. No MT hangs.

**Exercises**: `release()` waiter/vulture iteration, `alt_state` CAS from
release, cleanup in `prialt_begin_impl` phase 3.

**Complements TLA+**: ChannelLifecycle spec proves no lost waiter; this test
verifies the wakeup result values and cleanup are correct.

#### A3. Concurrent endpoint release

Create a channel, copy writer to N MTs on different Ps, copy reader to M
MTs. Have all endpoints close simultaneously. Verify no double-free (ASan),
no leak (channel count returns to baseline).

**Exercises**: `alive_` two-phase delete, refcount decrement ordering.

**Complements TLA+**: ChannelLifecycle spec proves `ExactlyOneDelete`; this
test verifies memory safety of the delete.

#### A4. Early wake path

Force the sequence: MT sets `suspending_=true`, waker sets
`wake_pending_=true` via `schedule()`, MT reaches `wake_pending_.exchange`
in `run(Status::detach)` and catches it before context-switching. Verify
MT stays on local queue and `drain_suspended` is a no-op.

**Exercises**: the early-wake fast path in `run()`.

**Complements TLA+**: DrainSuspended spec models this path (CheckWP action
with `wake_pending=TRUE`); this test verifies the C++ code matches.

#### A5. Timer heap boundary

Spawn 65 MTs that all `sleep_until` the same near-future deadline.
Verify all 65 complete. Exercises the 64-element `expired[]` array in
`fire_timers` — the 65th timer must be deferred to the next iteration.

**No TLA+ counterpart** — this is a boundary condition in the
implementation, not a protocol property.

#### A6. Implicit re-init

Call `init_runtime(4)` twice without `shutdown_runtime()`. Verify the
second init is clean: no leaked threads (join completes), no stale global
queue entries, fresh processor state.

**No TLA+ counterpart** — lifecycle management, not a concurrent protocol.

### B. Invariant-checking tests (`test/invariants.test.cc`)

These embed runtime assertions that check invariants *during* execution,
not just at the end. They catch transient violations that self-correct
before convergence.

#### B1. Run queue DLL consistency

Under a `#ifdef CSP_DEBUG_INVARIANTS` guard, add a `verify_dll()` helper
that walks the busy list and asserts: every node's `next_->prev_ == node`,
`prev_->next_ == node`, and the list is finite (no infinite loop). Call it
at entry/exit of `run()`, `schedule_local()`, `deschedule()`. Run a
volume workload with this instrumentation.

**Catches**: stale `next_`/`prev_` pointers after steal, double-link,
dangling link after deschedule.

#### B2. No double-enqueue

Assert in `push_to_global` that `!mt->in_global_` (already present as
`assert`). Assert in `schedule_local` that `!next_` (already present as
early return). Add a counted check that these assertions fire zero times
across a stress workload.

#### B3. Suspending window invariant

Under `CSP_DEBUG_INVARIANTS`, assert that while `suspending_=true`, the
MT has `next_ == nullptr` (not on local queue) and `in_global_ == false`
(not on global queue). Check in `schedule()` and `drain_suspended()`.

**Complements TLA+**: DrainSuspended spec's `TypeOK` and `NoLostWakeup`
verify this at the protocol level; this check verifies it in the C++.

### C. Deterministic interleaving tests (`test/interleave.test.cc`)

These use barriers and condition variables to force specific thread
interleavings. They require narrow test hooks (`#ifdef CSP_TEST_HOOKS`)
at specific protocol points.

#### C1. drain sees wake_pending

Hook into `drain_suspended` just before `global_mu.lock()`. In the hook,
signal a barrier, then wait for a second barrier. The test thread sets
`wake_pending_=true` via `schedule()` between the two barriers. Verify
the MT ends up on the global queue after drain completes.

**Directly validates**: the interleaving that the TLA+ DrainSuspended spec
proves safe (states 4-7 in the correct model trace).

#### C2. drain does NOT see wake_pending

Hook into `drain_suspended` just after releasing `global_mu`. In the
hook, signal the waker to call `schedule()`. Since `suspending_` is
now false, the waker should push the MT directly to the global queue.
Verify the MT is on the global queue and `wake_pending_` is false.

**Directly validates**: the other timing path in DrainSuspended.

#### C3. steal_work skips running MT

Hook into `steal_work` just before checking `victim.running`. Set up
the victim P with two MTs: one running (set as `running`), one idle.
Verify the thief steals only the idle MT.

**Directly validates**: the `running` pointer guard in StealWork.

#### C4. Surplus P wind-down

For testing, expose a shorter wind-down timeout (e.g., 100ms instead of
5s via a test-mode flag). Create a watchdog-induced surplus P. Drain
all work. Wait for the wind-down timeout. Verify `p.alive = false` and
the worker thread has exited. Then spawn new work and verify it still
runs (on the remaining Ps).

---

## Part 3: Relationship Between TLA+ and Code Tests

```
TLA+ Spec                    Code-Level Test
─────────────────────────    ─────────────────────────
Proves protocol design       Verifies implementation
Exhaustive (all states)      Probabilistic (one run)
Abstract (flags, queues)     Concrete (memory, threads)
No memory model              TSan checks memory ordering
No error paths               Tests allocation failures
No boundary conditions       Tests array bounds, overflow

             Together
    ─────────────────────────
    Protocol is correct AND
    implementation is faithful
```

Each TLA+ spec should have at least one code-level test that exercises
the exact interleaving the spec models. This creates a *correspondence
chain*:

1. TLA+ spec proves the protocol is safe for all interleavings
2. Code test forces a specific interleaving and verifies the C++ behaves
   as the TLA+ action predicts
3. Volume/stress test verifies correctness under real concurrency at scale
4. TSan verifies memory ordering the TLA+ model abstracts away

If a code test fails but the TLA+ spec passes, the implementation
diverges from the model. If both fail, the protocol itself is wrong.

---

## Priority Order

### Phase 1: Core scheduling protocols (highest risk)

| Item | Type | Effort | Risk addressed |
|------|------|--------|----------------|
| StealWork.tla | TLA+ | Medium | Dual-lock correctness, no lost/duplicated MTs |
| A1. steal_work contention | Code | Medium | Real contention, TSan validation |
| C3. steal skips running MT | Code | Small | Running pointer guard |
| ChannelLifecycle.tla | TLA+ | Medium | Endpoint delete, waiter wakeup |
| A2. Concurrent close + alt | Code | Medium | Close-during-sleep interaction |
| A3. Concurrent release | Code | Small | alive_ two-phase delete |

### Phase 2: Parking and shutdown (medium risk)

| Item | Type | Effort | Risk addressed |
|------|------|--------|----------------|
| WorkerParking.tla | TLA+ | Small | Shutdown CV race proof |
| A6. Implicit re-init | Code | Small | Stale state after re-init |
| C4. Surplus P wind-down | Code | Medium | Dynamic P pool lifecycle |

### Phase 3: Alt/channel protocol (lower risk, already well-tested)

| Item | Type | Effort | Risk addressed |
|------|------|--------|----------------|
| AltStateCAS.tla | TLA+ | Small | Multi-waker claim exclusivity |
| C1. drain sees wake_pending | Code | Medium | DrainSuspended correspondence |
| C2. drain doesn't see wp | Code | Medium | DrainSuspended correspondence |
| A4. Early wake path | Code | Small | run() fast path |

### Phase 4: Boundary conditions (low risk)

| Item | Type | Effort | Risk addressed |
|------|------|--------|----------------|
| A5. Timer heap boundary | Code | Small | 64-element array overflow |
| B1. DLL consistency | Code | Medium | Transient queue corruption |
| B3. Suspending invariant | Code | Small | Protocol-to-code correspondence |

---

## Files to create

| File | Purpose |
|------|---------|
| `formal/StealWork.tla` | Work-stealing protocol spec |
| `formal/StealWork.cfg` | TLC config |
| `formal/StealWork_Bug.tla` | Buggy variant (no running check or no dual-lock) |
| `formal/ChannelLifecycle.tla` | Channel close/delete protocol |
| `formal/ChannelLifecycle.cfg` | TLC config |
| `formal/ChannelLifecycle_Bug.tla` | Buggy variant (no alive_ guard or no CAS) |
| `formal/WorkerParking.tla` | Park/shutdown protocol |
| `formal/WorkerParking.cfg` | TLC config |
| `formal/AltStateCAS.tla` | Multi-waker claim protocol |
| `formal/AltStateCAS.cfg` | TLC config |
| `test/protocol.test.cc` | Tests A1-A6 |
| `test/invariants.test.cc` | Tests B1-B3 |
| `test/interleave.test.cc` | Tests C1-C4 |
