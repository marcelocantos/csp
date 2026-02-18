# A TOCTOU Race in M:N Microthread Suspension and its Verification with TLA+

> **Note:** Source file references (e.g. `src/csp.cc`) refer to the development
> source tree. In the amalgamated distribution, these live in `csp.cpp`.

## Abstract

We describe a time-of-check-to-time-of-use (TOCTOU) race that arises in
the suspension protocol of an M:N microthread scheduler, where a
microthread suspending on a channel and a waker calling `schedule()` on
its behalf can interleave in a way that permanently loses the wakeup. We
present the protocol that eliminates the race, construct TLA+
specifications for both the correct and buggy variants, and use the TLC
model checker to exhaustively verify correctness and reproduce the bug.

## 1. Context

CSP is a C++ microthreading library implementing typed synchronous
channels. Microthreads (MTs) are multiplexed across OS threads in an M:N
arrangement: N microthreads run on M OS worker threads, with
cooperative context switching within each worker and work-stealing across
workers.

When a microthread performs a channel operation (read or write) and no
peer is ready, it must *suspend* — remove itself from its worker's local
run queue and context-switch to another microthread. Later, when a peer
arrives on another thread, that peer calls `schedule()` to wake the
suspended MT by pushing it onto the global run queue.

The challenge is that suspension is not instantaneous. Between the moment
the MT decides to suspend and the moment it actually context-switches
out, there is a window during which the MT is still executing on its
worker thread but has already made itself visible to potential wakers
(by registering on a channel's wait queue). If a waker calls
`schedule()` during this window, the scheduler must defer the wakeup
rather than push the MT onto a run queue — pushing a still-executing MT
would cause double execution.

## 2. The suspension protocol

Two boolean flags on each microthread coordinate suspension:

```
std::atomic<bool> suspending_{false};
std::atomic<bool> wake_pending_{false};
```

Three participants interact:

- **The suspending MT** — sets `suspending_` to true, releases channel
  locks, then context-switches out via `do_switch(Status::detach)`.
  Inside `do_switch`, the MT checks `wake_pending_`: if a wakeup arrived
  early, the MT re-adds itself to the local run queue and returns without
  switching.

- **The waker** — a peer on another thread that finds the MT on a
  channel's wait queue and calls `schedule()`. Under `global_mu`,
  schedule checks `suspending_`: if true, it sets `wake_pending_` and
  returns; if false, it pushes the MT to the global queue directly.

- **The drainer** — after the MT context-switches out, the thread that
  resumes (the worker running the next MT) calls `drain_suspended()` on
  the now-suspended MT. This clears `suspending_` and drains any deferred
  `wake_pending_`.

The protocol in the source code (`src/csp.cc`):

```cpp
// drain_suspended — called after context switch completes
static void drain_suspended(Microthread* suspended) {
    auto& rt = Runtime::instance();
    std::lock_guard<std::mutex> lk(rt.global_mu);      // (A)
    suspended->suspending_.store(false, release);        // (B)
    if (suspended->wake_pending_.exchange(false, acq_rel)) {
        if (!suspended->in_global_)
            rt.push_to_global(suspended);                // (C)
    }
}

// schedule — called by waker on another thread
void Microthread::schedule(bool make_current) {
    auto& rt = Runtime::instance();
    std::lock_guard<std::mutex> lk(rt.global_mu);      // (D)
    if (suspending_.load(acquire)) {                     // (E)
        wake_pending_.store(true, release);              // (F)
        return;
    }
    rt.push_to_global(this);                             // (G)
}
```

The critical property: steps (A)-(C) and steps (D)-(G) are mutually
exclusive. The `global_mu` lock ensures that drain's clear-and-check
and schedule's check-and-set cannot interleave.

## 3. The TOCTOU race

Without the mutex, drain and schedule become sequences of independent
atomic operations that can interleave arbitrarily. The dangerous
interleaving occurs when drain checks `wake_pending_` *before* schedule
sets it:

| Step | Thread   | Action                       | State after              |
|------|----------|------------------------------|--------------------------|
| 1    | MT       | `suspending_ = true`         | susp=T, wp=F, queue=no   |
| 2    | MT       | context-switch out           | susp=T, wp=F, queue=no   |
| 3    | Drainer  | read `wake_pending_` → false | susp=T, wp=F, queue=no   |
| 4    | Waker    | read `suspending_` → true    | susp=T, wp=F, queue=no   |
| 5    | Waker    | `wake_pending_ = true`       | susp=T, wp=T, queue=no   |
| 6    | Drainer  | `suspending_ = false`        | susp=F, wp=T, queue=no   |

**Final state**: `suspending_` is false (drain completed),
`wake_pending_` is true (never consumed), and the MT is on no run queue.
The wakeup is permanently lost. The MT will never execute again.

The root cause is a classic TOCTOU: the drainer checks `wake_pending_`
(time of check) and later clears `suspending_` (time of use), but
between these two non-atomic steps, the waker observes `suspending_` as
still true and routes its wakeup through `wake_pending_` — which the
drainer has already checked and dismissed.

Note that the *order* of drain's two operations matters. If drain
cleared `suspending_` first and then checked `wake_pending_`, no race
would occur in this specific two-thread scenario (the waker would see
`suspending_=false` and push directly to the queue). However, the mutex
solution is strictly stronger: it is correct regardless of the internal
ordering of drain's operations, and it remains correct if additional
threads or protocol steps are added.

## 4. TLA+ model

We model the protocol as a TLA+ specification with six state variables:

```tla
VARIABLES
    suspending,     \* Boolean: MT has signaled it's suspending
    wake_pending,   \* Boolean: deferred wakeup
    on_queue,       \* Boolean: MT is on some run queue
    global_mu,      \* "none" | "drain" | "sched" — mutex holder
    pc_mt,          \* MT's program counter
    pc_waker        \* Waker's program counter
```

### 4.1 Granularity decisions

Each TLA+ action corresponds to a sequence of C++ operations that are
either genuinely atomic (a single `std::atomic` operation) or
effectively atomic (multiple operations under the same lock, invisible
to other threads). This is the key modeling judgment:

- **BeginSuspend** (set `suspending_`, leave queue): one action because
  the flag is set before any unlock — no other thread can observe the
  intermediate state.

- **Drain** (clear `suspending_`, check+clear `wake_pending_`, push to
  queue): one action because all operations are under `global_mu`.

- **DoSchedule** (check `suspending_`, conditionally set `wake_pending_`
  or push to queue): one action because all operations are under
  `global_mu`.

- **CheckWP** (atomically exchange `wake_pending_`): one action because
  `exchange` is indivisible.

### 4.2 Safety invariant

```tla
NoLostWakeup ==
    (pc_mt = "done" /\ pc_waker = "done_waker") => on_queue
```

When both threads have completed their protocols, the MT must be on a
run queue. If this invariant is violated, a wakeup was lost.

### 4.3 Verification results

**Correct specification** (`DrainSuspended.tla`): drain and schedule
both acquire `global_mu`. TLC explores all 17 reachable states (depth
8). Both `TypeOK` and `NoLostWakeup` hold in every state.

```
Model checking completed. No error has been found.
23 states generated, 17 distinct states found, 0 states left on queue.
```

**Buggy specification** (`DrainSuspended_Bug.tla`): drain does not
acquire `global_mu`; its check of `wake_pending_` and its clear of
`suspending_` are separate non-atomic actions. TLC finds the
`NoLostWakeup` violation in 8 steps:

```
Error: Invariant NoLostWakeup is violated.

State 4: <DrainCheckWP>      — drain reads wake_pending=FALSE
State 7: <DoSchedule>        — schedule sees suspending=TRUE,
                                sets wake_pending=TRUE
State 8: <DrainClearSusp>    — drain clears suspending,
                                never re-checks wake_pending

Final: on_queue=FALSE, wake_pending=TRUE — wakeup lost.
```

This matches the interleaving described in Section 3.

## 5. Faithfulness

A formal model is only as useful as its correspondence to the real code.
We address faithfulness at three levels:

**Action-to-code mapping.** Each TLA+ action is annotated with the
specific source file, function, and line range it models. For example,
the `Drain` action cites `csp.cc:51-56` (`drain_suspended` under
`global_mu`). A reader can verify the correspondence by inspection.

**Atomicity justification.** Every action that groups multiple
operations documents *why* the grouping is valid — typically because
the operations are under the same lock, or because they precede any
unlock that would make intermediate states visible.

**Conservative abstraction.** The model includes the early-wakeup path
(where `wake_pending_` arrives before the MT context-switches, checked
inside `run()`) and the post-suspension drain path. These are the two
paths through which a wakeup can be consumed. The model does not include
paths irrelevant to the invariant (timer expiry, I/O readiness, work
stealing), which only add ways to *schedule* an MT — they cannot cause a
lost wakeup for the specific suspend-wake pair being modeled.

**Limitations.** The model covers one MT and one waker. It does not
model multiple concurrent wakers, multiple suspensions in sequence, or
the `in_global_` guard. These are valid extensions for future work, but
the core TOCTOU race is a two-thread phenomenon, and the model captures
its essential structure.

## 6. Observations

**The mutex is cheap.** `global_mu` is held for approximately 3 atomic
operations (one store, one exchange, one conditional push). Contention
occurs only when a waker calls `schedule()` at the exact moment another
thread is draining — a narrow window. In practice, the critical section
takes tens of nanoseconds.

**The early-wakeup path is critical.** The `wake_pending_` check inside
`run()` (before context-switching) handles the case where the waker
arrives after `suspending_` is set but before the MT switches out. This
is conceptually a separate optimization (avoiding a needless context
switch), but it also carries safety implications: if the exchange in
`run()` consumes the pending wakeup, drain will not find it.

**TLA+ found the bug instantly.** TLC explored all 20 reachable states
in the buggy model in under a second. For concurrent protocols of this
size, exhaustive model checking is effectively free and strictly
dominates manual reasoning about interleavings.

## 7. Files

| File | Description |
|---|---|
| `formal/DrainSuspended.tla` | Correct specification (mutex held) |
| `formal/DrainSuspended.cfg` | TLC config for correct spec |
| `formal/DrainSuspended_Bug.tla` | Buggy specification (no mutex in drain) |
| `formal/DrainSuspended_Bug.cfg` | TLC config for buggy spec |
| `src/csp.cc:45-65` | `drain_suspended()` implementation |
| `src/csp.cc:120-147` | `Microthread::schedule()` implementation |
| `src/csp.cc:176-242` | `Microthread::run()` with early-wakeup check |
| `src/channel.cc:314-324` | Suspension site in `prialt_begin_impl` |
