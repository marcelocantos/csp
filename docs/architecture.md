# CSP Architecture and Implementation

This document describes the internal architecture of the CSP microthreading
library: how microthreads are represented, how context switching works, how
channels synchronise communicating microthreads, and how the M:N scheduler
distributes work across OS threads.

## Table of Contents

1. [Microthread Representation](#1-microthread-representation)
2. [Context Switching](#2-context-switching)
3. [Run Queue and Scheduling](#3-run-queue-and-scheduling)
4. [Channel Implementation](#4-channel-implementation)
5. [The Alt/Prialt Protocol](#5-the-altprialt-protocol)
6. [M:N Runtime](#6-mn-runtime)
7. [Work Stealing](#7-work-stealing)
8. [Concurrency Control](#8-concurrency-control)
9. [Timer System](#9-timer-system)
10. [Lifecycle of a Microthread](#10-lifecycle-of-a-microthread)
11. [Stream Combinators](#11-stream-combinators)
12. [I/O Reactor](#12-io-reactor)
13. [Blocking Pool](#13-blocking-pool)
14. [Signal Delivery](#14-signal-delivery)

---

## 1. Microthread Representation

Each microthread is a `Microthread` struct (`csp_internal.h`)
allocated at the top of its own 32 KB stack:

```
Low address                                          High address
┌──────────────────────────────────────────┬──────────────────┐
│               Stack space                │   Microthread    │
│           (grows downward)               │    struct        │
└──────────────────────────────────────────┴──────────────────┘
                                           ↑ 16-byte aligned
```

The stack is an array of 16-byte-aligned `StackSlot` values, heap-allocated
via `new`. The `Microthread` is placement-constructed at the end of this
array, ensuring 16-byte alignment as required by ARM64 and Boost.Context.

Key fields:

| Field            | Type                      | Purpose                                  |
|------------------|---------------------------|------------------------------------------|
| `prev_`, `next_` | `Microthread*`            | Circular doubly-linked run queue          |
| `ctx_`           | `atomic<fcontext_t>`      | Saved execution context (SP, registers)   |
| `stk_`           | `StackSlot*`              | Pointer to base of stack allocation       |
| `alt_state`      | `atomic<uint32_t>`        | ALT_IDLE / ALT_WAITING / ALT_CLAIMED     |
| `in_global_`     | `bool`                    | Currently in the global run queue         |
| `suspending_`    | `atomic<bool>`            | In the unlock-to-switch window            |
| `wake_pending_`  | `atomic<bool>`            | Woken during `suspending_` window         |
| `id_`            | `size_t`                  | Monotonically increasing unique ID        |
| `status_`        | `char[32]`                | Human-readable debug description          |

A sentinel microthread `Processor::main` anchors each processor's run
queue. It uses the default constructor which creates a self-referential
DLL node (`prev_ = next_ = this`) and has no user stack.

---

## 2. Context Switching

Context switching uses Boost.Context's fcontext API, which saves and restores
the CPU register set and stack pointer without involving the kernel:

```
jump_fcontext(target_ctx, data)
  → saves current registers + SP into an implicit context
  → loads registers + SP from target_ctx
  → returns transfer_t{saved_ctx, data} on the target's stack
```

The library wraps this in `switch_to(Microthread& mt, intptr_t data)`:

```
switch_to(target, data):
    self = g_self
    ctx = target.ctx_.load(acquire)          // (a) acquire target context
    current_p().save_ctx = &self->ctx_       // (b) where to store our context
    current_p().save_mt  = self              // (c) who is being suspended
    t = jump_fcontext(ctx, data)             // (d) context switch
    // --- resumed by someone jumping to us ---
    save_ctx->store(t.fctx, release)         // (e) store caller's context
    drain_suspended(save_mt)                 // (f) clear suspension state
    return t.data
```

Steps (a) and (e) form an acquire-release pair. When an OS thread saves a
microthread's context via release-store, any other OS thread that later
acquire-loads `ctx_` is guaranteed to see the saved register data on the
microthread's stack. This is critical in M:N mode where a microthread may
resume on a different OS thread than it was suspended on.

The `save_ctx` and `save_mt` fields are stored in the `Processor` struct
(per-OS-thread state), not in the `Microthread`, because between the
`jump_fcontext` call and its resumption, the calling microthread's struct
may be on another thread's stack. Using processor-local storage avoids
cross-thread data races.

---

## 3. Run Queue and Scheduling

### Local Run Queue

Each processor maintains a **circular doubly-linked list** (DLL) of runnable
microthreads. The `busy` pointer is the head of the queue; the sentinel
`main` is always present.

```
            busy
             ↓
  ┌──→ sentinel ←──→ mt_A ←──→ mt_B ←──→ mt_C ──┐
  └───────────────────────────────────────────────┘
```

All DLL mutations are protected by `Processor::run_mu`. The key operations:

- **`schedule_local()`**: Insert a microthread into the DLL (no-op if
  already linked, detected by `next_ != nullptr`).
- **`deschedule()`**: Remove a microthread from the DLL, nulling its links.
- **Inline deschedule/schedule in `run()`**: When context-switching from
  one microthread to another, both the deschedule of the old and the
  schedule of the new happen in a single critical section inside `run()`,
  avoiding the overhead of separate lock acquisitions.

### Scheduling States

A microthread transitions through these scheduling states:

```
         schedule_local()            local_next()
   [not linked] ───────────→ [in DLL] ──────────→ [running]
        ↑                        ↑                      │
        │                        └──────────────────────┘
        │                            yield / sleep
        │         deschedule (exit/detach)
        └─────────────────────────────────────────────┘
```

The `Status` enum drives transitions inside `run()` and `do_switch()`:

| Status   | Meaning                                         |
|----------|-------------------------------------------------|
| `run`    | Normal switch (yield). Caller stays in the DLL. |
| `sleep`  | Block on I/O. `busy` advances past the caller.  |
| `detach` | Suspend for channel wait. Caller is delinked.   |
| `exit`   | Microthread finished. Caller is delinked and its stack is passed to the target for deallocation. |

### do_switch and run

`do_switch(Status)` is the main entry point for context switching from a
microthread's perspective. It selects the next target from the DLL and calls
`target->run(status)`:

```
do_switch(status):
    lock run_mu
        running = g_self           // protect active MT from work stealing
        if busy == g_self:         // advance past self
            busy = busy->next_
        target = busy
    unlock run_mu
    target->run(status)
```

`run(Status)` performs the actual context switch. Under `run_mu`, it:

1. Delinks `g_self` from the DLL (for detach/exit).
2. Links `this` (the target) into the DLL (if not already present).
3. Releases the lock.
4. Calls `switch_to(*this, killme)` to transfer control.

On return from `switch_to` (when someone later switches back to this frame),
`run()` processes any `killyou` pointer (a dead microthread whose stack can
now be freed) and restores `g_self`.

---

## 4. Channel Implementation

### Structure

A `Channel` (`channel.cc`) contains:

- **`delegate_`**: Self-pointer at offset 0 (anticipates future channel
  fusion).
- **`id_`**: Unique ID for lock ordering.
- **`alive_`**: Atomic counter (starts at 2, one per endpoint side). When
  both sides reach zero, the channel deletes itself.
- **`mu_`**: Protects the waiters and vultures queues.
- **`endpts_[2]`**: Writer (index 0) and reader (index 1) endpoints,
  each containing:
  - `refcount`: Atomic reference count for that endpoint.
  - `waiters`: `RingBuffer<ChanopWaiter>` of microthreads ready to
    send/receive.
  - `vultures`: `unordered_set<ChanopWaiter>` of microthreads waiting
    for endpoint closure.
- **`tx_`**: The copy/move function for transferring messages.

### Endpoint Encoding

Writer and reader handles are pointers to the Channel struct with low bits
used as flags:

```
csp_writer = (Channel*) & ~15          // bit 0 = 0 → writer
csp_reader = (Channel*) | 1           // bit 0 = 1 → reader
```

Additional bits encode wait mode (ready vs. dead) and alive flags. The
`chan()` function strips the low bits to recover the Channel pointer.

### Reference Counting

Each endpoint (writer side, reader side) has an independent refcount.
`writer<T>` and `reader<T>` C++ wrappers call `addref` on copy and
`release` on destruction. When an endpoint's refcount reaches zero:

1. Lock the channel mutex.
2. Wake all waiters and vultures on the **opposite** side (via CAS on
   `alt_state`), signalling that their peer is gone.
3. Decrement the shared `alive_` counter. If it reaches zero (both
   endpoints are dead), delete the channel.

The two-phase alive counter prevents a race where both endpoints close
concurrently on different threads: each `release` decrements `alive_`
atomically, and only the last one performs the delete.

---

## 5. The Alt/Prialt Protocol

`prialt` is the core synchronisation primitive. It implements a three-phase
protocol:

### Phase 1: Scan for Ready Peer

```
Sort unique channels by id (lock ordering)
Lock all channels

For each chanop (in priority order, rotated by offset for alt):
    If opposite-side waiters queue is non-empty:
        CAS peer.alt_state: ALT_WAITING → ALT_CLAIMED
        Transfer message via tx_()
        Schedule the peer (push to global queue or run directly)
        Unlock all
        Return index
```

The CAS on `alt_state` ensures that exactly one waker can claim a sleeping
microthread. If the CAS fails, another thread already claimed it.

When a ready peer is found, message transfer happens directly between the
two microthreads' message slots, under the channel locks. The waking
microthread is then scheduled and the locks are released.

In single-processor mode, the woken peer is run immediately via
`run(Status::run)`, giving synchronous rendez-vous semantics. In M:N mode,
the peer is pushed to the global run queue via `schedule()`.

### Phase 2: Register and Sleep

If no peer is ready:

```
Set alt_state = ALT_WAITING
Register on each channel's waiters or vultures queue
Set suspending_ = true
Unlock all channels
do_switch(Status::detach)        // context switch away
suspending_ = false
```

The `suspending_` flag is set **before** unlocking. This is critical: after
the unlock, a waker on another OS thread could immediately find this
microthread in a channel's waiters queue and call `schedule()`. If the
microthread hasn't finished its context switch yet (the `do_switch` hasn't
completed), running it would cause double execution. The `suspending_` flag
tells `schedule()` to set `wake_pending_` instead of pushing to the global
queue. After the context switch completes, `drain_suspended()` checks
`wake_pending_` and pushes to the global queue if set.

### Phase 3: Cleanup

When the microthread is woken:

```
Lock all channels (same sorted order)
Remove self from all waiters/vultures queues
Unlock all
Set alt_state = ALT_IDLE
Return signal_
```

The `signal_` field was set by the waker in Phase 1. Positive values indicate
which chanop matched; negative values indicate endpoint closure.

### Lock Ordering

All channel locks are acquired in order of `Channel::id_` (a monotonically
increasing counter). This prevents deadlock when a microthread waits on
multiple channels simultaneously. The small-channel fast path uses a
fixed-size array of 8 pointers; larger alt sets spill to a heap-allocated
vector.

---

## 6. M:N Runtime

The M:N runtime maps G microthreads onto P processors running on M OS
threads, following the GMP model (similar to Go's runtime).

### Initialisation

`init_runtime(num_procs)` creates:

- **P processors** (`Processor` structs), each with its own local run queue,
  timer heap, and mutex.
- **P-1 worker threads**, each bound to a processor (P1..Pn). P0 is the
  main thread.
- The main thread's scheduler is set to `main_loop()`, which parks until
  `live_gs` reaches zero.

### Worker Loop

Each worker thread runs `worker_loop()`:

```
while not stopping:
    fire_timers(p)                    // reschedule expired timers
    if mt = local_next(p):            // try local run queue
        mt->run()
        continue
    if take_from_global(p):           // try global queue
        continue
    if steal_work(p):                 // try stealing from another P
        continue
    park(p)                           // sleep until work arrives
```

### Global Run Queue

The global run queue (`Runtime::global_run_queue`) is a `std::deque`
protected by `Runtime::global_mu`. It serves as the primary distribution
mechanism: newly spawned microthreads and woken microthreads (from channel
operations) are pushed here, and workers pull from it.

`take_from_global` transfers a fair share (total / num_procs, at least 1)
from the global queue to the local run queue via `schedule_local()`.

### Parking

When a worker has no work, it parks on `park_cv` with a predicate:

```
park_cv.wait(lock, [&] {
    return stopping || has_work(p);
});
```

`has_work` checks the local queue, global queue, and timer heap. Workers
also support `wait_until` with the next timer deadline.

The `unpark_one()` function wakes parked workers (currently via
`notify_all`). It is called after pushing to the global queue, after
successful work stealing, and after timer expiry.

### Shutdown

`shutdown()` sets `stopping = true`, briefly locks `park_mu` to synchronise
with any worker that is between checking the predicate and entering `wait()`,
then calls `notify_all()` and joins all worker threads.

---

## 7. Work Stealing

When a worker's local queue is empty and the global queue is also empty,
it attempts to steal work from another processor:

```
steal_work(thief):
    for each victim processor (skipping self):
        lock victim.run_mu
        try_lock global_mu              // non-blocking to avoid deadlock
        if !locked: skip

        candidate = victim.busy->prev_  // steal from tail
        if candidate is sentinel or busy or running:
            skip

        delink candidate from victim's DLL
        push_to_global(candidate)       // both locks held: atomic
        unlock both
        unpark_one()
        return true

    return false
```

### Safety Invariants

Three categories of microthreads must not be stolen:

1. **The sentinel** (`victim.main`): Anchors the DLL; never runnable.
2. **The DLL head** (`victim.busy`): About to be picked by `local_next`.
3. **The active microthread** (`victim.running`): Currently executing on the
   victim's OS thread. Its context hasn't been saved yet, so switching to
   it from another thread would cause double execution.

The `running` pointer is maintained in two places:

- **`local_next()`** sets `running` to the candidate it returns (the initial
  pick from the worker loop).
- **`do_switch()`** sets `running = g_self` under `run_mu` before selecting
  the next target. This keeps `running` current as execution chains through
  microthreads via yield, channel operations, and exit.

### Lock Ordering

`steal_work` acquires `victim.run_mu` first, then `global_mu` via
`std::try_to_lock`. This avoids deadlock with `take_from_global`, which
holds `global_mu` and then acquires `run_mu` (via `schedule_local`). If
`try_to_lock` fails, the thief skips that victim and tries the next.

By holding both locks during the delink-and-push sequence, the stolen
microthread is never in a state where `next_ == nullptr` and
`in_global_ == false` simultaneously---preventing `schedule()` on another
thread from seeing inconsistent state.

---

## 8. Concurrency Control

### Atomic Fields and Their Roles

| Field                | Ordering        | Purpose                              |
|----------------------|-----------------|--------------------------------------|
| `Microthread::ctx_`  | acquire/release | Cross-thread context visibility      |
| `alt_state`          | CAS (seq_cst)   | Exclusive wakeup claim               |
| `suspending_`        | acquire/release | Prevent premature scheduling         |
| `wake_pending_`      | acq_rel exchange| Deferred wakeup during suspension    |
| `in_global_`         | (under mutex)   | Prevent duplicate global queue entry |
| `Runtime::stopping`  | acquire/release | Shutdown coordination                |
| `Runtime::live_gs`   | acq_rel         | Track active microthread count       |
| `EndPoint::refcount` | acq_rel         | Endpoint lifecycle                   |
| `Channel::alive_`    | acq_rel         | Channel deallocation                 |

### The Suspension Protocol

The interaction between channel unlock and context switch creates a TOCTOU
window:

```
Thread A (suspending)          Thread B (waking)
─────────────────────          ──────────────────
suspending_ = true
unlock_all()
                               CAS alt_state → CLAIMED
                               schedule(mt_A):
                                 sees suspending_ == true
                                 sets wake_pending_ = true
                                 returns (does NOT push to global)
do_switch(detach)
  ... context switch ...
drain_suspended(mt_A):
  lock global_mu
  suspending_ = false
  if wake_pending_:
    push_to_global(mt_A)
  unlock global_mu
```

`drain_suspended` executes under `global_mu`, making it mutually exclusive
with `schedule()` (which also acquires `global_mu`). This eliminates the
race where `schedule()` sets `wake_pending_` concurrently with
`drain_suspended` clearing `suspending_` and checking `wake_pending_`.

### Lock Hierarchy

The following partial order is maintained to prevent deadlock:

```
Channel locks (sorted by id)
    └── global_mu
         └── run_mu (via schedule_local from take_from_global)

run_mu
    └── global_mu (via try_to_lock in steal_work only)
```

Channel locks are acquired in `Channel::id_` order. `global_mu` is acquired
after channel locks (in `schedule()`) and before `run_mu` (in
`take_from_global`). `steal_work` reverses the run_mu/global_mu order but
uses `try_to_lock` to avoid deadlock.

---

## 9. Timer System

Timers are per-processor min-heaps of `(deadline, Microthread*)` pairs:

```cpp
struct TimerEntry {
    steady_clock::time_point deadline;
    Microthread* thread;
};
// std::priority_queue with std::greater<> for min-heap
```

**`csp_sleep_until(deadline_ns)`**: Pushes the current microthread onto the
local processor's timer heap, sets `suspending_ = true`, and calls
`do_switch(Status::detach)`. On wakeup, clears `suspending_`.

**`fire_timers()`**: Called at the top of the worker loop. Pops all expired
entries from the timer heap and calls `schedule_local()` for each.

**Parking integration**: When a worker parks, it uses `wait_until` with the
next timer deadline (if any), ensuring timers fire even when there is no
other work.

**High-level API**: `sleep()`, `after()`, and `tick()` are thin wrappers.
`after()` and `tick()` are implemented as producer microthreads that
sleep and then write to a channel, making timers composable with `alt`.

---

## 10. Lifecycle of a Microthread

### Spawn

```
csp_spawn(entry_f, data):
    Allocate 32KB stack
    Placement-construct Microthread at top of stack
    make_fcontext(start, stack_top)     // create initial context
    switch_to(mt, &start_data)          // warmup handshake
    g_self = self                       // restore caller's identity
    live_gs++
    push_to_global(mt)                  // M:N mode
    notify workers
```

The warmup `switch_to` enters the microthread's `start()` function, which
copies `StartData` (entry function, data pointer, caller reference) to local
variables, then switches back to the spawner. This ensures the microthread
has valid state even after the spawner's stack frame is gone.

### Execution

A worker picks the microthread from the global queue, adds it to its local
DLL, and calls `mt->run()`. This resumes the microthread in its `start()`
function after the warmup switch. The user function runs until it blocks
(channel op, timer, yield) or returns.

### Exit

When the user function returns (or throws), `start()` calls
`do_switch(Status::exit)`. The exit path:

1. `do_switch` selects the next target from the DLL.
2. `target->run(Status::exit)`: delinks the exiting microthread from the
   DLL, links the target, and calls `switch_to(target, killme)` with the
   exiting microthread as `killme`.
3. The target's context resumes. It receives `killme` (a dying microthread)
   and destroys it: calls the destructor, deletes the stack.
4. Decrements `live_gs`. If it reaches zero, notifies `park_cv` to wake the
   main thread.

The `killme`/`killyou` handoff ensures the exiting microthread's stack is
not freed while it is still in use. The stack is freed by the *next*
microthread to run, which by definition is on a different stack.

### Same-Thread Migration

When a microthread is stolen from one processor's DLL and later picked up by
a different worker, the `switch_to` mechanism transparently handles the
cross-thread migration. The `ctx_` acquire/release pair ensures the new OS
thread sees the saved register state. Thread-local state (`g_self`,
`current_p()`) is re-evaluated on each function entry, so the microthread
naturally adapts to its new host thread.

---

## 11. Stream Combinators

Stream combinators are header-only templates that spawn internal
microthreads pre-wired to channel endpoints. Each combinator follows a
consistent pattern:

```cpp
// Core function: takes input reader, output writer, and parameters.
template<typename T, typename F>
auto map(reader<T> in, writer<U> out, F&& f) {
    for (T v; prialt(~out, in >> v) > 0;) {
        out << f(v);
    }
}

// Spawn variants: create channels and spawn the core function.
reader<U> spawn_map(reader<T> in, F&& f);   // downstream
writer<T> spawn_map(writer<U> out, F&& f);  // upstream
channel<T> spawn_map(F&& f);                // bidirectional
```

The `prialt(~out, in >> v)` pattern is idiomatic: it blocks until either
the output writer dies (downstream closed, returns non-positive) or input
data arrives (returns positive). This gives each combinator automatic
cleanup when either side of the pipeline is torn down.

Combinators compose naturally because their interfaces are just channels:

```cpp
auto r = spawn_producer<int>([](writer<int> w) { ... });
r = spawn_where(std::move(r), predicate);
r = spawn_map(std::move(r), transform);
r = spawn_buffer(std::move(r), 16);
```

Each `spawn_*` call adds a microthread to the pipeline. The microthreads
coordinate through synchronous channel operations, with backpressure
propagating naturally through the blocking send/receive semantics.

---

## 12. I/O Reactor

The I/O reactor (`reactor.h`, `reactor.cc`) provides non-blocking file
descriptor readiness notification, allowing microthreads to suspend on I/O
without stalling their processor.

### Architecture

The reactor is a singleton (`Reactor::instance()`) running a kqueue event
loop on a dedicated OS thread. This thread is not a Processor---it does not
run microthreads. Its sole purpose is to monitor file descriptors and
reschedule waiting microthreads when I/O readiness events fire.

```
Microthread                    Reactor thread              Global queue
    │                              │                           │
    │ wait_read(fd, mt)            │                           │
    ├─────────────────────────────→│                           │
    │ do_switch(detach)            │ kevent() blocks           │
    │                              │ fd ready                  │
    │                              │ mt->schedule()            │
    │                              ├──────────────────────────→│
    │                              │                      push mt
    │                              │                    unpark_one()
    │ ←─── worker picks up mt ─────┼───────────────────────────│
```

### Lazy Initialisation

`ensure_started()` uses double-checked locking: the fast path reads
`running_` with acquire; the slow path locks `start_mu_`, creates the
kqueue descriptor, registers an `EVFILT_USER` event (ident 0) for shutdown
wakeup, and spawns the reactor thread.

### Event Registration

`wait_read(fd, mt)` and `wait_write(fd, mt)` register a kevent with
`EV_ADD | EV_ONESHOT`. The microthread pointer is stored in the kevent's
`udata` field. `EV_ONESHOT` semantics mean each registration fires at most
once---the caller must re-register for subsequent waits. This avoids stale
event accumulation and simplifies cancellation.

`cancel(fd)` removes both read and write registrations for a descriptor.
Errors are ignored since the fd may not be registered for both filters.

### Suspension Protocol

The I/O wait primitives (`io_wait_readable`, `io_wait_writable`) follow
the same suspension protocol as channel operations:

```
io_wait_readable(fd):
    suspending_ = true           // (1) before reactor sees us
    reactor.wait_read(fd, self)  // (2) register with kqueue
    do_switch(detach)            // (3) context switch away
    suspending_ = false          // (4) resumed
```

Step (1) must happen **before** step (2). Once the kevent is registered,
the reactor thread can call `mt->schedule()` at any moment. If the
microthread has not yet completed its context switch (step 3), the
`suspending_` flag tells `schedule()` to set `wake_pending_` instead of
pushing directly to the global queue. After the context switch completes,
`drain_suspended()` checks `wake_pending_` and pushes the microthread if
set. This is the same TOCTOU prevention mechanism used by the channel path
(see [Section 8](#8-concurrency-control)).

### Event Loop

The reactor thread runs a tight loop calling `kevent()` with a 64-event
output buffer and no timeout (blocking indefinitely). For each event:

- `EVFILT_USER` events are skipped---they exist only to break the
  `kevent()` block during shutdown.
- All other events extract the `Microthread*` from `udata` and call
  `mt->schedule()`, which pushes the microthread to the global run queue
  and calls `unpark_one()` to wake a worker.

`EINTR` is handled by retrying the `kevent()` call.

### Shutdown

`shutdown()` sets `stopping_` with release ordering, then triggers the
`EVFILT_USER` event via `wake()` (`NOTE_TRIGGER`). This breaks the reactor
out of its `kevent()` block. The reactor thread checks `stopping_` at
the top of its loop and exits. The caller then joins the thread and closes
the kqueue descriptor.

### Layer 2: I/O Wrappers

The `csp::io` namespace (`io.h`) provides non-blocking wrappers around
standard POSIX calls (`read`, `write`, `accept`, `connect`). Each wrapper
retries on `EINTR`, and on `EAGAIN`/`EWOULDBLOCK` it calls
`wait_readable`/`wait_writable` to suspend the microthread until the
descriptor is ready, then retries the syscall. This gives callers
synchronous-looking I/O semantics while cooperating with the scheduler.

---

## 13. Blocking Pool

The blocking pool (`blocking_pool.h`, `blocking_pool.cc`) offloads
blocking OS calls to a dedicated thread pool so that microthreads can
invoke them without stalling their processor.

### Motivation

Some operations---DNS resolution (`getaddrinfo`), synchronous file I/O,
third-party library calls---cannot be made non-blocking. Running them
directly on a processor thread would stall all microthreads on that
processor. The blocking pool solves this by detaching the microthread from
its processor, running the blocking function on a pool thread, and
rescheduling the microthread when it completes.

### Architecture

The pool is a singleton (`BlockingPool::instance()`) with a fixed number of
worker threads (`max(4, hardware_concurrency)`). Workers spend most of their
time blocked in the kernel, not consuming CPU, so a relatively large pool
is cheap.

```
Microthread                  Pool thread              Global queue
    │                            │                        │
    │ submit(mt, fn)             │                        │
    ├───────────────────────────→│                        │
    │ do_switch(detach)          │ fn()  (blocking)       │
    │                            │ mt->schedule()         │
    │                            ├───────────────────────→│
    │                            │                   push mt
    │ ←── worker picks up mt ────┼────────────────────────│
```

### Submission

`submit(mt, fn)` pushes a `Work{mt, fn}` pair onto a `std::vector`-based
queue (LIFO order) and signals the condition variable. LIFO ordering gives
better cache locality for bursty submission patterns.

### Suspension Protocol

The public `csp::blocking(fn)` primitive (and the internal `run_blocking`)
follows the same suspension pattern as the reactor:

```
run_blocking(fn):
    suspending_ = true
    pool.submit(self, fn)
    do_switch(detach)
    suspending_ = false
```

As with I/O waits, `suspending_` is set before `submit` because the pool
thread can call `mt->schedule()` immediately upon completion. The
`suspending_`/`wake_pending_`/`drain_suspended` protocol prevents the
microthread from being pushed to the global queue before its context switch
completes.

### Return Value Forwarding

The public `csp::blocking<Fn>(fn)` template (`blocking.h`) supports
arbitrary return types. For non-void return types, it captures the result
in a local variable via a lambda wrapper, so the blocking function's return
value is available to the microthread when it resumes. For void functions,
no wrapper is needed.

### Worker Loop

Each pool thread blocks on a condition variable waiting for work or a
shutdown signal. When work arrives, it pops from the back of the queue,
runs `fn()`, then calls `mt->schedule()` to reschedule the microthread.

### Shutdown

`shutdown()` sets `stopping_` under the queue mutex and calls
`cv_.notify_all()`. Workers exit when they find the queue empty and
`stopping_` is true. All threads are joined and the queue is cleared.
Like the reactor, startup and shutdown are protected by double-checked
locking on `running_` with a `start_mu_` mutex.

---

## 14. Signal Delivery

Signal delivery (`signal.h`, `signal.cc`) converts asynchronous POSIX
signals into CSP channel events using the self-pipe trick.

### The Problem

POSIX signal handlers run asynchronously in an unspecified context and are
severely restricted in what they can safely do. Most library functions,
mutex operations, and memory allocations are forbidden. The signal delivery
system bridges this gap by having the handler perform only async-signal-safe
operations, with all complex logic running in microthreads.

### Self-Pipe Trick

Each call to `csp::signal::notify({SIGINT, SIGTERM, ...})` creates a pipe
and returns a `reader<int>` that emits the signal number each time a
matching signal arrives:

```
Signal handler               Pipe                 Producer MT          Channel
     │                         │                       │                  │
     │ write(byte)             │                       │                  │
     ├────────────────────────→│                       │                  │
     │                         │ io::read(buf)         │                  │
     │                         ├──────────────────────→│                  │
     │                         │                       │ out << signo     │
     │                         │                       ├─────────────────→│
```

The signal handler writes the signal number (as a single byte) to the pipe.
A producer microthread reads bytes from the pipe (using the I/O reactor for
non-blocking reads) and writes them to the output CSP channel.

### Lock-Free Signal-Safe Data Structures

The handler must avoid locks entirely. The design uses a fixed-size global
array of `SigPipe` structs:

```cpp
struct SigPipe {
    int write_fd;
    std::atomic<uint64_t> sig_mask;  // bitmask of signals this pipe cares about
};
SigPipe g_sig_pipes[MAX_SIG_PIPES];  // fixed array, no allocation
std::atomic<int> g_sig_pipe_count;   // number of active pipes
```

Lock-free atomics are async-signal-safe (they compile to single instructions
on modern architectures, with no possibility of deadlock). The handler reads
`g_sig_pipe_count` with acquire ordering, then checks each pipe's `sig_mask`
with acquire ordering, and calls `write()` on matching pipes. Both atomic
loads and `write()` are async-signal-safe.

### Memory Ordering

The ordering constraints form two acquire-release pairs:

1. **Registration (release) / handler (acquire) on `g_sig_pipe_count`**:
   When `notify()` stores a new pipe's `write_fd` and `sig_mask`, it
   publishes them by incrementing `g_sig_pipe_count` with release ordering.
   The handler's acquire load of `count` guarantees it sees the fully
   initialised `write_fd` and `sig_mask`.

2. **Cleanup (release) / handler (acquire) on `sig_mask`**: When the
   sentinel microthread clears a pipe's `sig_mask` to zero with release
   ordering, subsequent handler invocations that acquire-load `sig_mask`
   see zero and skip the pipe, preventing writes to a closed fd.

### Sentinel Microthread Pattern

Each `notify()` call spawns two microthreads:

- **Producer**: Reads bytes from the pipe read end via `io::read()` and
  writes signal numbers to the output channel. Runs until EOF (pipe write
  end closed) or the output channel dies.

- **Sentinel**: Watches for either the output reader dying (`~out_copy`)
  or the producer exiting (`~kill_r`, triggered when the producer's
  `kill_w` is destroyed). When either event fires, the sentinel:
  1. Clears `sig_mask` to zero (release), stopping the signal handler from
     writing to this pipe.
  2. Closes the pipe write fd, causing the producer's `io::read()` to
     return EOF and exit cleanly.

```
notify() creates:

  Producer MT                 Sentinel MT
  ─────────────               ─────────────
  holds: kill_w               holds: ~out_copy, ~kill_r
  loop:                       prialt(~out, ~kill):
    io::read(rfd) → out         clear sig_mask
    if out dead → return        close(wfd)
    (kill_w destroyed)          (producer sees EOF)
```

This two-microthread pattern ensures clean shutdown regardless of which
side initiates teardown:

- **Reader dropped**: `~out_copy` fires in the sentinel, which closes the
  pipe write end. The producer gets EOF and exits. Its `kill_w` is
  destroyed, but the sentinel has already exited via `~out_copy`.
- **Producer exits first**: `kill_w` is destroyed, so `~kill_r` fires in
  the sentinel, which closes the write fd and clears the mask.

### macOS Pipe Safety

On macOS, the pipe write end is configured with `F_SETNOSIGPIPE` via
`fcntl`. This prevents `SIGPIPE` delivery when `write()` is called on a
pipe whose read end has already been closed---a race that can occur between
the signal handler writing to the pipe and the producer closing the read
end during shutdown.

### Handler Installation

Signal handlers are installed via `sigaction` with `SA_RESTART` (to avoid
`EINTR` in unrelated syscalls). Installation is idempotent per signal
number and protected by `g_sig_mu` (a regular mutex, used only outside the
handler). Handlers remain installed after the pipe is cleaned up; they
become harmless no-ops since no pipe's `sig_mask` includes the signal.
