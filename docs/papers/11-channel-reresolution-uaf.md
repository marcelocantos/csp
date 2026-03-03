# The Channel Re-Resolution Use-After-Free

A use-after-free in `prialt_begin_impl` triggered by the interaction
between channel swap topology changes, endpoint death, and the
lock-free re-resolution window in the prialt retry path.

## Background

When an imp enters `prialt_begin_impl`, it resolves each `ChanOp`'s
channel pointer by reading through the endpoint's `Slot`:

```
slot->channel.load(acquire)  →  Channel*
```

This re-resolution runs **without holding any channel locks** (the
locks haven't been acquired yet — we need the channel pointers to
determine lock ordering).  After re-resolution, the function collects
unique channels, sorts them by ID, and calls `lock_all()`.

Between re-resolution and `lock_all()`, the channel pointers are raw,
unprotected `Channel*` values on the stack.

## The race

### Actors

1. **Child imp** (Thread B): sleeping on channel C_orig via prialt.
   Woken by a swap signal (`INT_MIN`), completes Phase 3 (deregister,
   unpin), enters retry path.
2. **Parent imp** (Thread A): executing a 4-argument `swap()` that
   performs two sequential `swap_slots` calls, then destroys local
   endpoint handles.

### Sequence

```
Thread B (child)                    Thread A (parent)
─────────────────                   ──────────────────
Wake from swap signal
Phase 3: lock, deregister,
         unlock, unpin
                                    swap_slots(S_ar, S_r)
goto retry                            S_r → C_new  (was C_orig)
re-resolve: read S_r→channel
  = C_orig  ← STALE!               swap() returns
                                    ~r1: reader_release(S_ar)
                                      → endpoint death on C_orig
                                        alive_ 2→1
                                    ~w2: writer_release(S_bw)
                                      → endpoint death on C_orig
                                        alive_ 1→0 → delete C_orig
collect channels, sort
lock_all(): C_orig->mu_.lock()
            ^^^^^^^^^^^^^^^^
            USE-AFTER-FREE
```

The child reads `slot->channel` and gets `C_orig`.  Before it can lock
`C_orig`, a concurrent swap redirects the slot to `C_new`, and the old
channel's endpoints are destroyed.  `C_orig` is freed.  The child then
dereferences the dangling pointer in `lock_all()`.

### Why the existing stale check doesn't help

The stale check (lines 294–310) runs **after** `lock_all()`.  By that
point the UAF has already occurred — the freed channel's mutex was
dereferenced to acquire the lock.

### Why the sleeping-waiter pin doesn't help

The `alive_` pin added in Phase 2 (lines 400–402) protects channels
during **sleep**.  After waking and completing Phase 3, the pins are
removed (lines 432–441).  The retry path's re-resolution runs
**after** unpin, in the unprotected window.

## Diagnosis

**ASan** (ubuntu-latest): SEGV at `dist/csp.cpp:638` reading
`cw.chanop->message` inside `prialt_begin_impl`.  Crash address
`0x03e900000dd9` — clearly freed/poisoned memory.  Triggered
consistently by `mn.test.cc:940` (StackPoolExhaustion).

**TSan** (ubuntu-latest): `heap-use-after-free` on `pthread_mutex_lock`
at the channel's `mu_` address.  The "previous write" trace shows
`operator delete` from `on_endpoint_death_locked` via
`reader_release` → `reader<int>::~reader()` at `swap.test.cc:945`.
The "read" trace shows `prialt_begin` → `chan_op<int>::operator bool()`
at `swap.test.cc:939`.

The TSan report was the key: it showed the **mutex itself** being
accessed after the channel was freed, confirming the race is between
re-resolution and `lock_all()`, not in the Phase 1 waiter scan.

## Why `chan_op<T>` is vulnerable

`chan_op<T>` stores raw `Channel*` (in `waiter.ptr`) and `Slot*`
pointers.  It does **not** hold a refcount on the Slot.  The
writer/reader handle that created the `chan_op` must outlive it for the
Slot's refcount to remain positive.  This is normally satisfied (the
handle is in scope during prialt), but the channel the Slot
**currently points to** can change via swap — and the **old** channel
can die if all its endpoints are released.

## Fix: slot-level spinlock with alive\_ pinning

### Invariant

At the moment `slot->channel.load()` executes, the channel is alive:
our Slot has `refcount > 0` (the handle is in scope), so our endpoint
on the channel is alive, so `alive_ ≥ 1`.  The problem is that between
the load and using the pointer, a swap can redirect the Slot and the
old channel can die.

### Mechanism

Add a spinlock (`std::atomic_flag`) to `Slot`.  Serialize the
"read channel + pin" operation with the "write channel" operation in
`swap_slots`:

```
Re-resolution (prialt_begin_impl):    swap_slots:
  slot->lock()                          slot->lock()
  ch = slot->channel.load()             slot->channel.store(new_ch)
  ch->alive_.fetch_add(1)    ← pin     slot->unlock()
  slot->unlock()
```

Under the slot lock, the channel pointer is stable and the channel is
alive (reasoning above).  The pin (`alive_++`) prevents deletion after
the lock is released.  When the channel is no longer needed (after
`lock_all()` succeeds, or on return), the pin is removed
(`alive_--`), possibly triggering deferred deletion.

### Pin lifecycle

| Path | Pin added | Pin removed |
|---|---|---|
| Stale → retry | re-resolution | top of retry |
| Dead/nowait/null | re-resolution | before return (after `unlock_all`) |
| Match (Phase 1) | re-resolution | `alt_end_impl` (after unlock) |
| Sleep (Phase 2) | re-resolution | after sleeping-waiter pins take over |
| Wake → retry | (sleeping-waiter) | Phase 3 unpin; re-resolution re-pins |
| Wake → return | (sleeping-waiter) | Phase 3 unpin |

### Deadlock freedom

Lock ordering: `swap_slots` acquires **channel mutexes** then **slot
spinlocks**.  Re-resolution acquires only **slot spinlocks** (no
channel mutexes).  Endpoint death acquires only **channel mutexes** (no
slot spinlocks).  No cycles exist.

## Affected tests

- `mn.test.cc:940` — StackPoolExhaustion (ASan, ubuntu-latest: consistent)
- `swap.test.cc:939` — MN Split split-while-reader-blocked (TSan, ubuntu-latest: intermittent)
- `mn.test.cc:361` — ManyChannelMessages (ASan, ubuntu-24.04-arm: intermittent)

All three are M:N mode tests with concurrent swap or high channel
contention.
