# 33 — Channel hot-path performance analysis

*2026-07-18. Measurement platform: M4 Max (16 cores), macOS 26, Clang -O2,
normal (non-sanitizer) build. Driver: two imps, one `chan<int>`, N
send/recv rendezvous (`bench/channel.bench.cc` `send/recv` shape).*

## Headline numbers

| Configuration | ns per rendezvous |
|---|---:|
| `CSP_MAXPROCS=2` (1 worker) | **321** |
| `CSP_MAXPROCS=3` | 841 |
| `CSP_MAXPROCS=4` | 1,396 |
| `CSP_MAXPROCS=8` | 3,544 |
| `CSP_MAXPROCS=16` (default here) | **5,081** |
| buffered `chan<int>(1024)`, 2 procs | 825 |
| buffered `chan<int>(1024)`, 16 procs | 11,250 |
| `chan<int>` create+destroy | 167 |

Two structural facts dominate everything else:

1. **Negative scaling: adding processors makes the same two-imp workload
   16× slower.** The workload is inherently serial (one producer, one
   consumer, unbuffered), so ideal behaviour is flat ns/op regardless of
   pool size. Instead each added P deepens the wake/migrate/park storm.
2. **A 1024-slot buffered channel is slower than an unbuffered one**
   (825 vs 321 ns at 2 procs; 11.2 µs vs 5.1 µs at 16). A buffer that
   size should decouple the endpoints almost completely; instead every
   element costs two full rendezvous plus a third imp's scheduling.

## Where the time goes (sample profiles)

At 16 procs, top-of-stack is almost entirely kernel synchronisation:
`__psynch_cvwait` (parked workers), `__psynch_mutexwait` (mutex
contention), `__psynch_cvsignal`/`__psynch_cvbroad` (wake traffic).
Call-tree attribution of the contended-mutex samples:

- `drain_suspended` (csp.cc:99) — **takes `global_mu` on every context
  switch**, unconditionally.
- `Imp::schedule` (csp.cc:190) — takes `global_mu` on **every wake**.
- `prialt_begin_impl` lock_all / phase-3 relock (channel.cc:354, :491).
- `worker_loop` take_from_global / steal_work / park transitions.

`__psynch_cvsignal` attributes to `unpark_one` (Note::wake) and to
`park_cv.notify_all` — which `unpark_one` calls on **every** invocation,
even when all workers are already busy.

At 2 procs (no contention), the per-op ~321 ns decomposes roughly as:
context switch (`jump_fcontext` + `Imp::run`/`do_switch` queue
manipulation under `run_mu`), pthread mutex lock/unlock pairs
(`run_mu`, `global_mu`, channel `mu_` — ~5–6 lock/unlock pairs per
rendezvous), TLS accessor calls (`current_imp` + `current_p` +
`_tlv_get_addr` sum to a top-3 bucket, comparable to the entire
`prialt_begin_impl` body), and the `prialt_begin_impl` machinery itself
(slot spinlock, pin/unpin, `std::sort` of one element, registration
ring-buffer traffic).

## Why the wake path collapses under more Ps

Every rendezvous wake follows this route (channel.cc → csp.cc →
runtime.cpp):

```
matcher finds sleeping peer
  → peer->make_runnable() → Imp::schedule()
      → lock global_mu, push_to_global(peer)      [global serialization]
      → unpark_one()
          → scan procs for a sleeping Note        [O(nprocs)]
          → Note::wake()                          [pthread cv signal → syscall]
          → park_cv.notify_all()                  [broadcast → syscall, EVERY call]
  → some OTHER worker wakes (futex round-trip)
      → take_from_global (global_mu again)
      → runs peer on a different core             [imp migrates, caches cold]
  → original worker's local queue is now empty
      → steal_work scan → park                    [more lock traffic + cv wait]
```

With one worker none of the cross-core traffic exists — the woken peer
lands in the same P's queue and the switch is direct. With 15 idle
workers, every rendezvous migrates the peer to a fresh core via two
syscalls and a global lock, then the abandoned worker parks again.
The kernel-visible cost (~2 psynch round-trips ≈ 3–4 µs) is the
difference between 321 ns and 5 µs.

There is no local-queue fast path for wakes: `schedule()` **always**
pushes to the global queue. Go's scheduler solves this exact shape with
`runnext` — the woken goroutine goes to the waker's own P's next slot,
and cross-P wakeups only happen when the waker's P is saturated.

## Ranked opportunities

### O1. Wake-to-local fast path (the big one)

When `make_runnable()` runs on a P-bound thread, put the woken imp in
the **current P's local run queue** (a `runnext`-style single slot or
the existing DLL) instead of the global queue, and skip `unpark_one`
entirely when the local queue was empty (the current P will reach it on
its next `do_switch`). Global queue + unpark stays for wakes from
non-P threads (reactor, blocking pool) and for overload spill.
Expected effect: the 16-proc curve flattens toward the 321 ns figure;
ping-pong stops migrating cores. This changes the work-conservation
story (a busy P could sit on a runnable imp while other Ps idle), so it
needs either a periodic spill rule (Go: runnext demotes to local queue,
stealable) or the watchdog as backstop — and a TLA+ update to
`StealWork`/`DrainSuspended` before implementation, per the repo's
formal-verification convention.

### O2. `drain_suspended` off the global lock

`global_mu` is acquired on **every context switch** solely to close the
suspending_/wake_pending_ TOCTOU race. A per-imp CAS state machine
(RUNNING / SUSPENDING / SUSPENDED with a WAKE_PENDING bit folded in —
i.e. collapse `suspending_` + `wake_pending_` into one atomic word)
removes the global lock from both `drain_suspended` and the
`schedule()` suspending-window check. `DrainSuspended.tla` already
models this protocol; extend it for the lock-free variant first.
Combined with O1, the rendezvous path touches no global lock at all.

### O3. `unpark_one` broadcast storm

`park_cv.notify_all()` fires on every `unpark_one` call — three call
sites in the function, including the "found a sleeping worker" success
path and the "everyone is busy" path. `park_cv` only matters to
`main_loop`/`run`/`quiescent_loop` (completion/quiescence watchers).
Gate the broadcast on a `main_parked_`/watcher-count flag so the
steady-state rendezvous path skips it. Also: `Note` advertises
"platform futex (__ulock_wait / futex / WaitOnAddress)" in its header
comment but actually implements only the mutex+condvar fallback —
implementing the futex path (single syscall wake, no mutex) cuts the
remaining wake cost roughly in half; alternatively fix the comment.

### O4. Buffered channels without the middleman imp

`chan<T>(N)` spawns a filter imp: every element is two rendezvous plus
a third schedulable entity. A ring buffer owned by the Channel itself
(push under `mu_` without sleeping while not full, wake reader only on
empty→non-empty edge, Go-hchan style) makes the uncontended buffered
send ~a mutex+memcpy (~20–50 ns) instead of 825–11,250 ns. This is the
largest single win available for throughput-oriented pipelines and
makes buffer capacity actually deliver decoupling. Cost: a second
channel kind inside `Channel` (the imp-based semantics — exception
forwarding, death propagation, `alt` integration — must be preserved;
the ring variant must register with the same waiter machinery so
`alt`/`prialt`/vultures still work). Substantial project; keep the
filter-imp as the semantic reference implementation and validate the
ring against it property-test-style.

### O5. Singleton-op fast path in `prialt_begin_impl`

`w << v` / `r >> v` (count==1, the dominant call shape) pays for the
general machinery: re-resolution pin loop, dedup scan, `std::sort` of
one element, `match_internal` bookkeeping, an unused
`std::vector<Channel*>` construction, and two atomic pin/unpin RMWs.
A specialised count==1 path — slot lock, resolve, pin, channel lock,
scan opposite waiters, transfer or register — skips the sort/dedup/copy
entirely. Modest but real (~tens of ns of the 321), and it shrinks the
instruction footprint of the most common call.

### O6. Cache `current_imp()` across `prialt_begin_impl`

`current_imp()`/`current_p()` are deliberately non-inline (TLS caching
bug, docs/tls-caching-bug.md) and together show up as a top-3 bucket in
the uncontended profile. `prialt_begin_impl` calls `current_imp()`
~15 times. The rule the TLS bug imposes is "don't cache across
`jump_fcontext`" — caching in a *local* between switch points is safe.
Hoist one `Imp* self = current_imp();` at entry, re-load once after
`do_switch` returns, and pass `self` into `EndPoint::wait`. Same
pattern applies to `do_switch` itself. Easy, low-risk, measurable.

### O7. Channel allocation (167 ns: 3 allocs + snprintf)

`make_chan` performs three separate `new`s (Channel + 2 Slots) and the
`descr_` field eagerly formats a `std::string` via snprintf. Co-locate
the two Slots with the Channel in one allocation (they die together —
`mem_release` refcounts already handle the lifetime split) and make
`descr_` lazy (format on first `describe()` call; it's a debug
facility). Matters for `call()`-style request/response, which creates
a channel per request. Also: `counterses()` refs/derefs/active atomics
fire on every endpoint copy/release into a single shared cache line;
they exist for `channel_count()` (a test/debug API) — worth gating or
relaxing to per-P counters.

### O8. Cheaper `alt` offset RNG

`alt_begin` draws from a thread_local `std::mt19937` through
`std::uniform_int_distribution` per call. A `thread_local` xorshift/PCG
word with Lemire reduction is ~5 ns vs ~20–30. Minor; bundle with
other work.

## Suggested sequence

O6+O3 (small, safe, immediate) → O2 (TLA first) → O1 (TLA first; the
scalability fix) → O5 → O7/O8 → O4 (separate objective, largest
engineering effort, largest throughput payoff).

With O1–O3+O5–O6 landed, a 16-proc rendezvous should approach the
~200–300 ns range (switch + channel mutex + transfer); O4 then takes
buffered pipelines to memcpy speed.

## Method note

`bench/channel.bench.cc` had bit-rotted (`csp_chanop`, a removed C-API
type) — fixed in the commit accompanying this paper, so `make bench`
runs again. The numbers above came from focused drivers rather than
nanobench because per-op attribution needed `sample`-able steady-state
runs (macOS `sample`, no sudo — see paper 32's method note).
