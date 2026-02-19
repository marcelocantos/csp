# Verifying a Microthread Scheduler with TLA+

## Abstract

We describe five TLA+ specifications that model concurrent protocols
in an M:N microthread scheduler: suspension handoff, work stealing,
channel lifecycle, alt-state CAS claiming, and worker parking. Each
protocol is modeled in both correct and intentionally buggy variants.
The TLC model checker exhaustively verifies the correct specifications
and reproduces specific interleaving failures in the buggy ones —
failures that ThreadSanitizer, AddressSanitizer, and stress testing
could not detect. We discuss the modeling choices, the bugs found, and
the practical value of lightweight formal verification for concurrent
systems code.

## 1. Why schedulers are hard to test

A microthread scheduler is a composition of multiple synchronization
protocols, each using its own combination of mutexes, atomics, and
condition variables. Each protocol is simple enough to fit on a screen.
The difficulty is that they interact: a wakeup produced by one
protocol is consumed by another, and the interleaving of steps across
OS threads can produce states that no single protocol was designed to
handle.

Standard testing tools have fundamental limitations here:

- **ThreadSanitizer** detects data races — concurrent accesses to the
  same memory location where at least one is a write and neither uses
  synchronization. But it does not reason about *protocol
  correctness*. If every individual access is properly synchronized
  (using atomics with correct memory ordering), TSan reports no
  errors — even if the composition of those accesses has a logic bug.

- **Stress testing** explores random interleavings, but the bug may
  require a specific sequence of 8 steps across 3 threads, with
  context switches at precise points. A stress test might run millions
  of iterations without hitting the one interleaving that matters.

- **Code review** is limited by the reviewer's ability to mentally
  simulate all possible interleavings. Humans are remarkably bad at
  this — we tend to verify the paths we can think of and miss the
  ones we can't.

TLA+ addresses this by exhaustively enumerating every reachable state
in a model. For protocols of this size (5-15 state variables, 10-20
actions), TLC explores the entire state space in under a second. If a
safety invariant can be violated, TLC finds the shortest path to the
violation.

## 2. The TOCTOU race in suspension

### 2.1 The protocol

When a microthread suspends (waiting for a channel peer), three
participants interact:

- The **suspending MT** sets `suspending_ = true` and context-switches
  out.
- A **waker** on another thread calls `schedule()`. Under `global_mu`,
  it checks `suspending_`: if true, it sets `wake_pending_ = true`
  and returns; if false, it pushes the MT to the global queue.
- The **drainer** — the thread that runs next after the context switch
  — calls `drain_suspended()` under `global_mu`. It clears
  `suspending_` and checks `wake_pending_`.

The critical property: `drain_suspended()` and `schedule()` both
acquire `global_mu`, making them mutually exclusive. This prevents a
time-of-check-to-time-of-use race.

### 2.2 The race

Without the mutex, drain and schedule become sequences of independent
atomic operations. The dangerous interleaving:

| Step | Thread  | Action                       | State                    |
|------|---------|------------------------------|--------------------------|
| 1    | MT      | `suspending_ = true`         | susp=T, wp=F, queue=no   |
| 2    | MT      | context-switch out           | susp=T, wp=F, queue=no   |
| 3    | Drainer | read `wake_pending_` → false | susp=T, wp=F, queue=no   |
| 4    | Waker   | read `suspending_` → true    | susp=T, wp=F, queue=no   |
| 5    | Waker   | `wake_pending_ = true`       | susp=T, wp=T, queue=no   |
| 6    | Drainer | `suspending_ = false`        | susp=F, wp=T, queue=no   |

Final state: `wake_pending_` is true but never consumed. The MT is
on no queue. The wakeup is permanently lost.

### 2.3 TLA+ verification

The correct spec (`DrainSuspended.tla`) models drain and schedule as
atomic actions under `global_mu`. TLC explores all 17 reachable
states at depth 8. The invariant `NoLostWakeup` — "when both threads
are done, the MT is on a queue" — holds in every state.

The buggy spec (`DrainSuspended_Bug.tla`) splits drain into two
non-atomic steps (check `wake_pending_`, then clear `suspending_`).
TLC finds the violation in 8 steps, reproducing the exact interleaving
above.

## 3. Work stealing

### 3.1 The invariant

In the M:N scheduler, idle workers steal microthreads from busy
workers. A critical safety property: **never steal the MT that is
currently executing.** The running MT stays on the victim's local
doubly-linked list (DLL) — it is not removed during execution. The
only thing that prevents it from being stolen is an explicit `running`
pointer on each processor.

### 3.2 The dual-lock protocol

Work stealing requires two locks:

1. `victim.run_mu` — protects the victim's local DLL and `running`
   pointer.
2. `global_mu` — protects the global run queue.

The thief acquires `victim.run_mu` first (blocking), then tries
`global_mu` (non-blocking `try_lock`). If `global_mu` is unavailable,
the thief releases `victim.run_mu` and moves on to the next victim.

While both locks are held, the thief:
1. Reads the victim's `busy` pointer (head of local DLL).
2. Selects a candidate MT from the DLL.
3. Checks `candidate != victim.running` — skipping the executing MT.
4. Delinks the candidate from the DLL.
5. Pushes it to the global queue.

The dual-lock hold ensures the stolen MT is never visible with
inconsistent state — it transitions atomically from "on victim's
local queue" to "on global queue."

### 3.3 TLA+ verification

`StealWork.tla` models three participants (Victim, Thief, Waker)
operating on a set of MTs. The spec tracks local queues, the global
queue, running pointers, and both locks.

Safety invariants:
- `NoLost`: every MT is on exactly one queue or being actively
  executed.
- `NoDuplicates`: no MT appears on more than one queue.
- `RunningNotStealable`: a running MT is never stolen.

`StealWork_Bug.tla` removes the `running` check. TLC finds a
`RunningNotStealable` violation where the thief steals the victim's
currently-executing MT.

## 4. Channel lifecycle

`ChannelLifecycle.tla` models the reference-counting protocol for
channel endpoints. Writers and readers independently release their
references; the channel is freed when both counts reach zero. The
model verifies that:

- A released endpoint never causes a use-after-free.
- The waker (which holds a reference during its operation) never
  accesses a freed channel.
- The cleanup sequence for writer release and reader release can
  interleave in any order and the channel is still freed exactly once.

Eighteen TLA+ actions model the complete lifecycle: both endpoints
releasing, waking peers, and final cleanup. The bug variant omits the
reference check before the waker accesses the channel, producing a
use-after-free.

## 5. Alt-state CAS

`AltStateCAS.tla` models the protocol by which multiple wakers compete
to claim a sleeping microthread registered on multiple channels. The
MT sets `alt_state = ALT_WAITING`. Each waker attempts a
compare-and-swap: `ALT_WAITING → ALT_CLAIMED`. Exactly one succeeds;
the rest observe the CAS failure and skip.

The model verifies:
- `ExactlyOneWaker`: at most one waker succeeds.
- `NoLostSignal`: if any waker exists, the MT is eventually claimed
  and scheduled.

Seven actions model the full protocol. The bug variant weakens the
CAS to a non-atomic read-then-write, producing double-scheduling.

## 6. Worker parking

`WorkerParking.tla` models the shutdown/wake protocol for worker
threads. Workers park on a condition variable (`park_cv`) when there
is no work. The `shutdown()` call sets a flag and notifies all
workers.

The subtle issue: a worker may have already evaluated the CV
predicate (returning false) but not yet entered `cv.wait()`. If
`notify_all` fires during this gap, the notification is lost for that
worker.

The fix is an empty critical section: `shutdown()` acquires `park_mu`
before calling `notify_all`. If a worker is between predicate
evaluation and `cv.wait` (still holding the lock), `shutdown` blocks
on `park_mu`. The worker enters `cv.wait`, atomically releasing the
lock. `shutdown` acquires, releases, and notifies — finding the
worker blocked.

The bug variant omits the `park_mu` acquisition. TLC finds a state
where `shutdown` has completed but a worker is permanently blocked in
`cv.wait`.

## 7. Bug variants as mutation tests

Each specification has a `_Bug` variant that removes exactly one
synchronization mechanism:

| Spec | Bug variant removes |
|---|---|
| `DrainSuspended_Bug.tla` | `global_mu` in drain |
| `StealWork_Bug.tla` | `running` pointer check |
| `ChannelLifecycle_Bug.tla` | reference check before waker access |
| `AltStateCAS_Bug.tla` | CAS atomicity (replaced with read-then-write) |
| `WorkerParking_Bug.tla` | `park_mu` acquisition in shutdown |

This is mutation testing applied to the formal model. The bug
variants serve two purposes:

1. **Validation of invariants.** If the bug variant passes (TLC finds
   no violation), the invariant is too weak — it doesn't capture the
   property we think we're verifying. Every bug variant in our suite
   produces a violation, confirming that the invariants are strong
   enough.

2. **Documentation of failure modes.** The error trace produced by TLC
   is a concrete, step-by-step interleaving that leads to the
   violation. A developer reading the trace understands exactly *how*
   the missing synchronization causes the bug, which is more
   informative than a code comment saying "this lock is necessary."

## 8. Bidirectional correspondence tags

A formal model is only as useful as its correspondence to the real
code. To make this verifiable, every TLA+ action is annotated with a
tag of the form `TLA:Module.Action`, and the corresponding C++ code
carries the same tag in a comment:

```tla
(* Schedule an MT that is not currently suspending.
 *
 * TLA:StealWork.WStartSchedule *)
WStartSchedule == ...
```

```cpp
// Push to global queue under global_mu.
//
// TLA:StealWork.WStartSchedule
void Microthread::schedule(bool make_current) { ... }
```

A verification script (`scripts/check_tla_tags.py`) scans both TLA+
specs and C++ source files, extracts all tags, and confirms
bidirectional coverage: every tag in a TLA+ spec has a corresponding
tag in the C++ code, and vice versa. This runs as part of the build
(`make check`), catching drift between the model and the
implementation.

## 9. What TLA+ catches and what it doesn't

### What it catches

- **Lost wakeups**: the TOCTOU race in `drain_suspended` was found
  instantly. TLC produced the exact 8-step interleaving.
- **Stolen running MT**: the missing `running` check in work stealing.
- **Shutdown hang**: the CV notification race in worker parking.
- **Double-scheduling**: weakened CAS in alt-state claiming.

These are all bugs that involve specific interleavings of
correctly-synchronized operations. Each individual atomic or mutex
is used properly; the bug is in the *protocol* — the composition
of multiple synchronized steps. TSan cannot detect these because
there is no data race at the memory-access level.

### What it doesn't cover

- **Memory ordering bugs**: TLA+ models assume sequential consistency.
  A bug caused by missing acquire/release ordering on ARM64 would
  not be found.
- **ABA problems**: the models use abstract values, not machine words.
  A counter wrapping to a previous value would not be detected.
- **Performance**: TLA+ says nothing about contention, cache pressure,
  or latency.
- **Faithfulness**: the model is a manual abstraction of the code. If
  the model doesn't match the code (wrong action boundaries, missing
  participants), the verification is meaningless. The bidirectional
  tags mitigate this but don't eliminate it.

### Cost/benefit

For protocols of this size (5-15 variables, 10-20 actions, 2-3
participants), TLC explores the full state space in under a second.
Writing a TLA+ spec takes roughly the same time as writing a detailed
comment block explaining the protocol — and produces machine-checked
guarantees instead of prose that might be wrong.

The five specifications described here — totalling roughly 500 lines
of TLA+ — collectively verify the safety properties of the scheduler's
suspension, stealing, lifecycle, claiming, and parking protocols. They
found bugs that would have been extraordinarily difficult to diagnose
through testing alone.
