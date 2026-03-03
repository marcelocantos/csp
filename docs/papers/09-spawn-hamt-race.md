# The Spawn HAMT Race and Catch-Block Migration Bug

Two concurrency bugs in CSP's M:N scheduler, both manifesting as
memory corruption under glibc's `MALLOC_PERTURB_=42` on Linux.
Bug #1 was a classic use-after-free across a context switch.  Bug #2
was a subtler interaction between C++ exception handling ABI state and
imp thread migration.

## Background

When a new imp is spawned, it inherits its parent's dynamic-scoping
context — a HAMT root pointer (`dyn_ctx_`).  The spawn protocol uses a
two-phase warmup handshake: the new imp's `start()` function runs
briefly to copy data from the parent's `StartData` struct, then
switches back to the parent, which pushes the new imp onto the global
run queue.  A worker thread later resumes the new imp from where it
left off.

## Bug #1: HAMT use-after-free (fixed)

`start()` copied the parent's `dyn_ctx_` value before the warmup
switch but called `hamt_retain()` *after* it — i.e., after the parent
had resumed and could freely release or modify its own `dyn_ctx_`.  In
M:N mode, the parent could exit a `context_scope` (or die entirely)
before the new imp was scheduled on a worker thread, freeing the HAMT
node.  The new imp then called `hamt_retain()` on freed memory.

**Diagnosis path.** The bug was invisible on macOS (Mach-O TLV
indirection changes timing) and on Linux without memory debugging.  It
was exposed by glibc's `MALLOC_PERTURB_=42`, which poisons freed
memory with a deterministic byte pattern.  Running under ASan inside
Docker (`--platform linux/amd64`) produced a clean
`heap-use-after-free` trace pointing to `hamt_retain` in `start()`.

**Fix.** Move the `hamt_retain()` call and the `dyn_ctx_` assignment
to before the warmup switch, while the parent is suspended and the
HAMT node is guaranteed alive.  `~Imp()` already calls
`hamt_release(dyn_ctx_)`, so the early retain is balanced if the imp
is destroyed before resuming.

## Bug #2: catch-block thread migration (fixed)

After fixing the HAMT race, the supervisor "restart under contention"
test (`test/supervisor.test.cc:208`) still crashed approximately 1 in
10 runs under `MALLOC_PERTURB_=42` on Linux.  The crash site was
`__cxa_end_catch` (null pointer dereference at address 0x0),
suggesting corrupted C++ exception runtime state.

### Root cause

`spawn_entry` (in `include/csp/csp.h`) performed a **channel send
inside a `catch (...)` block**:

```cpp
// OLD CODE — BUGGY
catch (...) {
    auto ex = std::current_exception();
    if (!(sd->w << ex) && ...) { std::terminate(); }
}
```

The channel send (`sd->w << ex`) evaluates `operator bool()` on a
`chan_op`, which calls `prialt_begin`.  If the supervisor hasn't
entered `alt()` yet, `prialt_begin` **suspends the imp** via
`do_switch(Status::detach)`.  In M:N mode, the imp can resume on a
*different* OS thread.

The C++ exception ABI maintains per-thread state in
`__cxa_eh_globals`, a thread-local structure.  `__cxa_begin_catch`
(at catch-block entry) registers the exception on the *current*
thread's `__cxa_eh_globals`.  `__cxa_end_catch` (at catch-block exit)
cleans up by popping from the *current* thread's state.  If the catch
block spans a thread migration:

1. `__cxa_begin_catch` runs on thread A — registers exception on A's
   TLS
2. `prialt_begin` → no match → yield → imp suspended
3. Imp resumes on thread B
4. `__cxa_end_catch` runs on thread B — tries to pop from B's
   `caughtExceptions` (empty or wrong entry) → **null pointer
   dereference**

### Fix

Move the channel send outside the catch block.  `std::exception_ptr`
is a refcounted handle to a heap-allocated exception object — it's
safe to use after the catch block ends:

```cpp
// FIXED CODE
std::exception_ptr ex;
try { ... }
catch (...) { ex = std::current_exception(); }
if (ex) {
    if (!(sd->w << ex) && ...) { std::terminate(); }
}
```

The same pattern was also present in `include/csp/part/try_map.h`
and was fixed there as well.

### Why `supervised_fn` wasn't affected

`supervised_fn::operator()` (in `src/imp_exit.cc`) already had the
correct pattern — its catch block only calls
`std::current_exception()`, with all channel operations outside:

```cpp
catch (...) { ex = std::current_exception(); }
// channel sends happen here, safely outside catch
```

### TLA+ verification

`formal/CatchBlockMigration.tla` (fixed) and
`formal/CatchBlockMigration_Bug.tla` (old code) model the
interaction.  The invariant is:

> While the worker is inside a catch block (including yielded states),
> `worker_thread` must equal `catch_thread` (the thread where
> `__cxa_begin_catch` ran).

TLC finds the violation in the buggy spec in 5 states:
`WorkerThrow` → `WorkerSendInCatchYield` → `SupervisorReady` →
`WorkerResumeInCatch("t2")` — at which point `catch_thread = "t1"`
but `worker_thread = "t2"`.

### Why MALLOC_PERTURB made this visible

Without `MALLOC_PERTURB_=42`, the channel send usually completes
immediately because the supervisor is already in `alt()` — the
match is found synchronously and no yield occurs.  With
`MALLOC_PERTURB_`, glibc's memory poisoning slows allocation paths
enough to change the scheduling: the supervisor hasn't entered
`alt()` yet when the worker tries to send, forcing the worker to
yield *inside* the catch block.

## General principle for M:N threading

**Never perform operations that can yield (channel sends/receives,
`csp::yield()`, `csp::sleep()`) inside C++ catch blocks** in M:N
mode.  The C++ exception handling ABI assumes catch blocks execute
entirely on a single OS thread.  Always capture the exception with
`std::current_exception()` inside the catch, then handle it outside.

This extends to any language runtime state that is thread-local:
signal masks, errno, thread-local allocator caches, etc.  The general
rule is that catch blocks (and any scope that interacts with
thread-local state) must not span a context switch.

## Related

- [01-tls-caching.md](01-tls-caching.md) — earlier TLS caching race
  in the same `start()` function
- [04-tla-verification.md](04-tla-verification.md) — TLA+ methodology
  used in the scheduler exit fix that preceded this investigation
- [06-dynamic-scoping.md](06-dynamic-scoping.md) — HAMT design and
  the refcounting protocol that the retain/release operates on

## Appendix: Diagnostic process

This bug was found through code-reading rather than the originally
planned TLA+ approach.  The path is worth documenting because it
illustrates how prior session context and a good hypothesis can
short-circuit a more systematic investigation.

### What the prior session established

The prior session (which found and fixed Bug #1) produced a paper
stub with a **hypothesis** for Bug #2:

> The `exception_ptr` may hold a reference to the exception object on
> the dying imp's stack.  If `destroy_imp` reclaims the stack before
> the supervisor copies or rethrows the exception, the `exception_ptr`
> dereferences freed memory.

This hypothesis was *wrong* about the mechanism (the exception object
is heap-allocated, not on the imp stack) but *correct* about the
symptom class (use-after-free of exception-related state during M:N
thread migration).  It also correctly identified the **cast of
characters**: worker imp exit, exception propagation, supervisor
wakeup.

### How the actual bug was found

Starting fresh with the restored context, the plan was to write TLA+
specs first.  But before writing TLA+, reading the code was needed to
scope the spec's state space.

While reading `spawn_entry` in `include/csp/csp.h:886-897`:

```cpp
catch (...) {
    auto ex = std::current_exception();
    if (!(sd->w << ex) && ...) { std::terminate(); }
}
```

The channel send `sd->w << ex` jumped out immediately.  The prior
session's analysis of the imp exit flow had laid out the steps:
"throws → catches → writes exception_ptr to spawn-handle channel."
Knowing that channel sends can suspend (from deep familiarity with
the prialt/channel machinery), the `<<` inside the catch block was
instantly recognizable as a yield point inside C++ exception ABI
scope.

The key insight came from **cross-referencing two pieces of knowledge**:
1. The C++ exception ABI uses per-thread state (`__cxa_eh_globals`)
   that `__cxa_begin_catch` and `__cxa_end_catch` must access on the
   same thread.
2. CSP's M:N scheduler can resume a suspended imp on any OS thread.

Neither fact alone is surprising.  The bug exists at their
intersection — and it was visible only because the prior session had
already narrowed the search to "exception handling during the spawn
exit path."

### Why the original hypothesis was wrong but useful

The hypothesis about `exception_ptr` referencing stack memory was
incorrect — `std::current_exception()` creates a heap-allocated copy.
But it directed attention to exactly the right code: the catch block
in `spawn_entry` where the exception is captured and sent.  Reading
that code with "what happens to exception state across thread
boundaries?" in mind made the actual bug obvious.

### Comparison: `supervised_fn` as control case

`supervised_fn::operator()` was also examined for the same pattern.
It turned out to already have the correct structure — channel sends
outside the catch block.  This served as a natural control: if
`supervised_fn` was safe and `spawn_entry` was not, the difference
had to be in the catch-block structure, confirming the diagnosis.

### Role of TLA+ (post-hoc)

The TLA+ specs were written *after* the fix rather than before.
Their value shifted from "diagnostic tool" to "documentation and
regression guard."  The bug spec's 5-state counter-example provides
a concise, machine-checked proof that the race exists in the old
code, and the fixed spec proves it's closed.

For this particular bug, code-reading was more direct because the
issue was a single-point mistake (yield inside catch) rather than a
multi-actor protocol race.  TLA+ would have found it too, but would
have required first correctly modeling the `__cxa_eh_globals`
thread-locality constraint — which requires the same cross-domain
insight that made the code-reading approach work.
