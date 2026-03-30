# Quiescence Scope Gap: Analysis and Proposed Fix

## Status

Open. The yield+recheck workaround is in production (665/665 tests pass,
29/30 stress clean). This document describes the proper fix.

## Problem

The `quiescence_scope` tracks how many imps are "active" (running or
runnable). The fake_clock hook advances time only when `active == 0`
(all scope members sleeping). There is a **gap** between an imp's
`leave()` (when it yields via `do_switch`) and `enter()` (when it
resumes after being scheduled). During this gap, `active` can
momentarily hit 0, causing the hook to fire prematurely — advancing
fake time while a pipeline imp is mid-transfer.

### The gap in detail

Consider two imps: Writer (W) and Reader (R), communicating via a
channel, both in a quiescence scope with a tick timer.

```
Timeline (single scenario):

1. W is running (active=2: W + R were both entered at spawn)
2. R sleeps waiting for data: do_switch → leave() → active=1
3. W writes to channel → prialt finds R as a match
4. alt_end_impl: R->schedule() → pushes R to global queue
5. W continues running (still active)
6. W sleeps (waiting for tick): do_switch → leave() → active=0  ← GAP
7. Worker picks up R, R resumes: enter() → active=1
8. R processes data, sleeps: leave() → active=0 (legitimate)
```

At step 6, `active == 0` but R is on the global queue (about to run).
The hook sees quiescence and advances time prematurely.

### Why the gap exists

The `enter()` call happens at **resume time** — when `do_switch`
returns after the imp is context-switched back in. But between
`schedule()` (step 4) and resume (step 7), the imp is "in flight"
(on the global queue, waiting for a worker). During this window,
the imp is not counted as active.

### The yield+recheck workaround

The hook rechecks `is_quiescent()` after a `std::this_thread::yield()`.
This gives the worker thread time to pick up the scheduled imp and
call `enter()`. It works in practice (~97-100% of serial runs) but
is theoretically unsound — a sufficiently slow worker (e.g., under
heavy CPU contention) could still miss the recheck window.

## Proposed fix: enter at schedule time, not resume time

### Core idea

Move `enter()` from the **resume path** (do_switch return) to the
**schedule path** (where the imp is made runnable). This eliminates
the gap: the imp is counted as active from the moment it's scheduled,
not from the moment it actually runs.

### Why previous attempts failed

Three attempts were made and all caused deadlocks or hangs:

#### Attempt 1: enter in `push_to_global()`

```cpp
void Runtime::push_to_global(Imp* imp) {
    if (imp->qs_entered_) imp->qs_->enter();  // ← added
    imp->in_global_ = true;
    global_run_queue.push_back(imp);
    ...
}
```

**Failed because**: `push_to_global` is called from multiple paths,
including `internal::spawn()`. At spawn time, the imp already had
`enter()` called (in the spawn function). The push_to_global enter
was a **double enter**. Each imp was entered twice but only left once
per sleep → active count grew unboundedly.

#### Attempt 2: enter in `alt_end_impl()` only

```cpp
if (mi->peer) {
    if (mi->peer->qs_entered_) mi->peer->qs_->enter();
    mi->peer->schedule();
}
```

**Failed because**: the resume-path enter was not removed. Each
channel-woken imp was entered twice (once by alt_end_impl, once
at resume) but only left once per sleep.

#### Attempt 3: enter at ALL schedule call sites, no resume enter

Entered in `alt_end_impl`, `fire_expired`, and `drain_suspended`.
Removed the resume-path enter. Removed the spawn-time enter (relying
on push_to_global).

**Failed because**: `Imp::schedule()` has early-return paths that
bypass `push_to_global`:

```cpp
void Imp::schedule(bool make_current) {
    auto& rt = Runtime::instance();
    {
        std::lock_guard<std::mutex> lk(rt.global_mu);
        if (in_global_) return;           // ← SKIP: already queued
        if (suspending_) {
            wake_pending_ = true;
            return;                        // ← SKIP: deferred
        }
        if (next_) return;                 // ← SKIP: in local queue
        rt.push_to_global(this);
    }
    rt.unpark_one();
}
```

When `schedule()` returns via `in_global_`, `next_`, or `suspending_`
paths, no `enter()` is called. But the imp IS going to run (it's
already queued or will be re-queued by drain_suspended). The enter
is missed. Later, when the imp resumes, it doesn't enter (resume
enter was removed). Active count is too low.

### The real fix: unify all schedule paths through a single chokepoint

The fundamental issue is that there are **multiple paths** by which
an imp transitions from sleeping to runnable, and they don't all go
through the same code:

| Path | How it works | Currently enters? |
|---|---|---|
| `alt_end_impl` → `schedule()` → `push_to_global` | Channel match wakes peer | At resume |
| `fire_expired` → `schedule()` → `push_to_global` | Timer fires | At resume |
| `drain_suspended` → `push_to_global` | Wake-pending cleared | At resume |
| `schedule()` → early return (in_global_) | Already queued | At resume |
| `schedule()` → early return (next_) | In local queue | At resume |
| `schedule()` → early return (suspending_) | Deferred to drain | At resume |
| `schedule_local()` | Direct local queue add | At resume |

All seven paths eventually lead to the imp running. All seven need
`enter()` to be called. Currently, `enter()` is called at a single
point: the resume path in `do_switch`. This is correct (one enter
per wake, balanced with one leave per sleep) but has the gap.

**The proposed fix**: create a single function that handles the
sleep→runnable transition, including both the scope enter and the
queue manipulation:

```cpp
// New function: atomically transition imp from sleeping to runnable.
void Imp::make_runnable() {
    if (qs_entered_) qs_->enter();
    // ... existing schedule logic (push_to_global or local) ...
}
```

Then replace ALL schedule paths to go through `make_runnable()`:
- `alt_end_impl`: call `peer->make_runnable()` instead of `peer->schedule()`
- `fire_expired`: call `imp->make_runnable()` instead of `imp->schedule()`
- `drain_suspended`: call `suspended->make_runnable()` instead of `push_to_global`
- `schedule_local`: add enter at the top

And remove the resume-path enter from `do_switch`.

### Critical invariant

**Every sleep→runnable transition must call `enter()` exactly once.**
Every runnable→sleep/exit transition must call `leave()` exactly once.

The tricky cases:

1. **`schedule()` early returns**: If the imp is already queued
   (`in_global_` or `next_`), it was already entered when it was
   first made runnable. The second `schedule()` call should NOT
   enter again. `make_runnable()` must check if the imp was already
   entered for this wake cycle.

2. **`suspending_` → `wake_pending_`**: The imp is in the
   unlock_all→do_switch window. It hasn't actually slept yet
   (`leave()` hasn't been called). When `drain_suspended` later
   processes wake_pending, it should NOT enter (the imp never left).
   `make_runnable()` needs to distinguish "wake from sleep" from
   "prevent sleep" (wake_pending case).

3. **Spawn**: A newly spawned imp enters the scope but hasn't slept
   yet. Its first `enter()` is at spawn time. It should NOT be
   entered again when pushed to global. `make_runnable()` should
   only enter if the imp previously called `leave()`.

### Proposed implementation

Add a flag `qs_sleeping_` to Imp:

```cpp
struct Imp {
    // ... existing fields ...
    bool qs_sleeping_ = false;  // true after leave(), false after enter()
};
```

In `do_switch` (leave path):
```cpp
if (self->qs_entered_) {
    self->qs_sleeping_ = true;
    self->qs_->leave();
}
```

In `make_runnable()`:
```cpp
void Imp::make_runnable() {
    if (qs_entered_ && qs_sleeping_) {
        qs_sleeping_ = false;
        qs_->enter();
    }
    // ... existing schedule logic ...
}
```

This ensures:
- `enter()` is only called if the imp previously `leave()`d (qs_sleeping_)
- `enter()` is called exactly once per wake (qs_sleeping_ cleared)
- Early returns in `schedule()` don't double-enter (qs_sleeping_ is
  already false after the first make_runnable)
- Spawn doesn't double-enter (qs_sleeping_ starts false)
- Wake-pending doesn't enter (qs_sleeping_ is false because the imp
  is still in the suspending window, hasn't called leave yet)

### Testing strategy

1. Remove the `std::this_thread::yield()` recheck workaround
2. Run 20x parallel stress test: `seq 20 | parallel -j20 /tmp/run_test.sh`
3. Run 100x serial stress test
4. If any test hangs, the enter/leave counting is wrong — add fprintf
   tracing of `active_` transitions to find the imbalance

### Files to modify

- `include/csp/internal/csp_internal.h`: add `qs_sleeping_` to Imp
- `src/csp.cc`:
  - `do_switch`: set `qs_sleeping_ = true` before `leave()`
  - Remove resume-path `enter()` after `target->run()` in `do_switch`
  - `internal::spawn`: don't set `qs_sleeping_` (imp hasn't slept)
- `src/channel.cc`:
  - `alt_end_impl`: call `make_runnable()` instead of `schedule()`
- `src/clock.cc`:
  - `fire_expired`: call `make_runnable()` instead of `schedule()`
- `src/runtime.cpp`:
  - `push_to_global`: no change (called by make_runnable)
  - `drain_suspended`: call `make_runnable()` instead of raw push

### TLA+ validation

The `formal/QuiescenceDeadline.tla` spec should be updated to model
`qs_sleeping_` and verify that the invariant (enter exactly once per
wake, leave exactly once per sleep) holds across all paths. The
existing spec is too simplified — it doesn't model the early-return
paths in `schedule()` or the `suspending_` window.

## Related files

- `formal/QuiescenceScope.tla` — abstract model (3 strategies)
- `formal/QuiescenceDeadline.tla` — deadline cancel scenario
- `include/csp/csp.h` — `quiescence_scope` class
- `src/csp.cc` — `do_switch`, `Imp::schedule`, `drain_suspended`
- `src/channel.cc` — `alt_end_impl`
- `src/clock.cc` — `fire_expired`, quiescence hook
- `src/runtime.cpp` — `push_to_global`, `main_loop`
