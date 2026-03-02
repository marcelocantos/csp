# The Spawn HAMT Race

*Status: stub — to be expanded once the remaining supervisor crash is
root-caused.*

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

## Bug #2: supervisor SIGSEGV (open)

After fixing the HAMT race, the supervisor "restart under contention"
test (`test/supervisor.test.cc:208`) still crashes approximately 1 in
10 runs under `MALLOC_PERTURB_=42` on Linux x86_64.  The crash site
is now `__cxa_end_catch` (null pointer dereference at address 0x0),
suggesting corrupted exception state rather than HAMT corruption.

### Hypothesis

The imp exit path during exception propagation involves:

1. Worker imp throws → `spawn_entry` catches → writes
   `exception_ptr` to spawn-handle channel → `do_switch(Status::exit)`
2. `run()` passes `killme = self` via `switch_to` → the receiving imp
   calls `destroy_imp(killyou)`, which runs `~Imp()` and returns the
   stack to the pool
3. The supervisor wakes from `alt()`, reads the `exception_ptr`, and
   respawns the worker

The `exception_ptr` may hold a reference to the exception object on
the dying imp's stack.  If `destroy_imp` reclaims the stack before
the supervisor copies or rethrows the exception, the `exception_ptr`
dereferences freed memory.

### Proposed TLA+ spec

Model four concurrent actors — worker imp, supervisor imp, killyou
chain, stack pool — with the safety invariant: *an imp's stack is not
in the free pool while any `exception_ptr` referencing objects on that
stack is still reachable.*  If the spec finds a violation, the
counter-example trace will show the exact interleaving.

## Related

- [01-tls-caching.md](01-tls-caching.md) — earlier TLS caching race
  in the same `start()` function
- [04-tla-verification.md](04-tla-verification.md) — TLA+ methodology
  used in the scheduler exit fix that preceded this investigation
- [06-dynamic-scoping.md](06-dynamic-scoping.md) — HAMT design and
  the refcounting protocol that the retain/release operates on
