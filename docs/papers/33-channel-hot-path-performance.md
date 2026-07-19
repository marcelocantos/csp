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

## Results (same day, 🎯T34 implementation)

O6+O8, O3, O2, O1, O5, O7-lite landed in sequence (commits `041110e`,
`9659527`, `73d7d54`, `1b54133`). After:

| Configuration | before | after |
|---|---:|---:|
| pingpong, 2 procs | 321 | **~185** |
| pingpong, 4 / 8 / 16 procs | 1,396 / 3,544 / 5,081 | **~189 (flat)** |
| buffered `chan<int>(1024)`, 2 / 16 procs | 825 / 11,250 | 498 / 2,571 |
| bench `send/recv` (16 procs) | 5,145 | 188–250 |
| bench `prialt/2ch` / `alt/2ch` (16 procs) | 4,259 / 5,631 | 417 / 499 |
| bench `alt/8ch` (16 procs) | 5,216 | ~5,920 (see below) |
| bench (all shapes) at 2 procs | — | 147–209 |

The negative scaling is gone: ns/op is flat in pool size, and the
16-proc rendezvous is 27× faster. Wake-to-local (O1) delivered the
structural change; O2 removed `global_mu` from every context switch;
O3 removed the broadcast storm and gave `Note` a real futex.

**Known trade — multi-writer fan-in at high proc counts.** `alt/8ch`
(8 always-ready writers, one reader) reads ~13% slower at 16 procs:
woken writers land on the reader's P (first) or spill to the global
queue (rest) instead of re-registering in parallel on remote cores.
At 2 procs the same shape runs at 209 ns — the multi-proc fan-in
number is residual scheduler churn, not new serialization. Accepted
in exchange for the 4–28× wins elsewhere; addressed by the next-round
items below.

## Next round (profiled after 🎯T34 landed)

Fresh `sample` profiles of the optimized build:

- **Ping-pong (190 ns) is now mutex-pair-bound**: ~4–6 uncontended
  pthread mutex lock/unlock pairs per rendezvous — channel `mu_`
  (lock_all in phases 1–3), `global_mu` (schedule placement still
  serializes there), `run_mu` twice per switch (`do_switch` +
  `Imp::run`). Candidates: `os_unfair_lock`/futex lock for channel
  `mu_` (short critical sections); a per-imp placement CAS to get
  `schedule()` off `global_mu` (completing acceptance criterion 2);
  merging `do_switch`/`run` queue manipulation into one `run_mu`
  section. `current_p()` (non-inline TLS, several calls per switch)
  deserves the O6 treatment — pass `Processor&` through.
  `jump_fcontext` is the intrinsic floor.
- **Fan-in is channel-`mu_`-contention-bound**: every `alt` over K
  channels locks all K channels for phase-2 registration and phase-3
  deregistration while writers hammer the same locks. A sticky-
  registration design (waiters stay registered across alt iterations;
  only the fired op re-arms) would remove most of that traffic — a
  significant architectural change; model it in TLA+ first.
- **O4 (ring-buffer buffered channels)** remains the largest
  throughput item: 2,571 ns at 16 procs vs a ~50 ns mutex+memcpy
  potential.

## Round 2 (same day): placement claim, FastMutex, current_p

Guided by the next-round profile above (commits `ec2f5db`, `e933098`,
`8ee3d4f`):

- **current_p() caching** — same rule as O6: the `Processor&` is
  stable between switch points and must be re-resolved after
  `jump_fcontext`; hoisted per region across the switch path.
- **Placement-claim CAS** — racing placers (duplicate wakers, the
  deferred-wake drain) claim `Imp::placed_` with one exchange; only
  the winner inserts. The wake path now touches **no global lock at
  all** unless it spills — closing acceptance criterion 2 of 🎯T34.
  Verified in `formal/PlacementClaim.tla` (AtMostOnce +
  ExactlyOnceAtEnd); the `_Bug` variant shows check-then-set double-
  placing. The big winner was fan-in: spilling wakers no longer
  serialize on `global_mu` — the alt/8ch driver went 6,451 →
  ~1,500 ns at 16 procs.
- **FastMutex** — channel `mu_`, `run_mu`, and `global_mu` switch to
  `os_unfair_lock` on macOS (std::mutex elsewhere; park_mu keeps its
  condvar mutex). A/B: uncontended ping-pong unchanged (Apple's
  pthread_mutex is already cheap uncontended); contended shapes gain
  15–20% (buffered chan(1024): 1,871–2,121 → ~1,670 ns).

Final acceptance sweep (`make bench`): `send/recv` **189.6 ns at
default 16 procs vs 191.9 ns at 2 procs** — ratio ≈1.0 against the
baseline's 16× gap. All shapes at 2 procs: 150–210 ns. The
multi-writer shapes at 16 procs remain noisy run-to-run (1.3–5 µs,
err 8–25%) — residual fan-in churn, next round's target. Oracles:
757/757 native, 739/739 TSan, 739/739 ASan+UBSan; TLC green on
DrainSuspended, PlacementClaim, StealWork, ParkGate (+ failing _Bug
counterparts).

The post-round-2 profile has no syscalls and no pthread mutexes left
on the hot path — remaining cost is the algorithm itself:
`prialt_begin_impl` (with inlined unfair locks), `jump_fcontext`
(intrinsic floor), run-queue DLL ops, and the non-inline TLS
accessors. Further gains need structural work: a count==1 fast path
that skips the pin/dedup machinery entirely, sticky alt registration
(fan-in), and O4 ring-buffer buffered channels.

## Round 3 (2026-07-19): instruction-level and assembly research

Micro-costs of the primitives on the M4 Max (scratchpad `prims`
driver, 2-proc runtime):

| Primitive | ns |
|---|---:|
| `current_imp()` / `current_p()` (non-inline TLS) | 1.5 / 1.9 |
| FastMutex pair / std::mutex pair (uncontended) | 2.2 / 4.7 |
| atomic exchange (acq_rel) | 0.8 |
| `jump_fcontext` (raw, per jump) | **11.6** |
| yield round-trip (full scheduler path) | 82.6 → **68.2** |

Fixes landed (commits `64e73e0`, `acb96bc`):

- **`record_stack_high_water` ran on every suspend** — a mutex + hash
  probe whose only consumer (spawn's `CSP_ANALYSE_STACKS` slot sizing)
  is compiled out by default. Now compiled out together. This was the
  single largest per-suspend parasite (~8 ns).
- DD death-trace guard: function-local static (init-guard check ×8
  sites per prialt) → namespace-scope const bool.
- `StackPool::maybe_shrink`: out-of-line no-op call ×2 per op in arena
  builds → header inline no-op.
- **`do_switch`/`Imp::run` critical-section merge**: the suspend path
  took `run_mu` twice per switch; now once, with the departure
  bookkeeping (delink, CheckWP, placement clear) inline. The
  main()-misuse check became a direct `self == &p.main` test at entry.
  `switch_to` takes the caller's `Imp*` instead of re-reading TLS.
  (One lesson: the first cut asserted `target->next_` *outside*
  `run_mu` — TSan caught the racy read against concurrent thieves;
  the assert moved inside the lock.)

Net: yield 82.6 → 68.2 ns; ping-pong ~166–169 ns at 2 and 16 procs.

**Assembly findings.** The vendored Boost fcontext ARM64 jump saves
d8–d15 + x19–x30 (+PC): 10 stp / 10 ldp — essentially the AAPCS
minimum for its contract, measured at 11.6 ns/jump. Porting the hot
C++ *to* assembly was evaluated and rejected: the channel/scheduler
cost is semantics-bound (required atomics + cache traffic that clang
already compiles near-optimally), and hand-ported code goes dark to
TSan/ASan — the load-bearing oracles for every scheduler change here.

The one asm project with measured headroom is redesigning the switch
*contract* rather than its instructions: a **minimal-save switch**
(asm saves only fp/lr/PC; the call site declares x19–x28 and the full
vector file as clobbers, so the compiler spills exactly the live
subset). Prototyped (`bench/lightswitch/`): **9.4 ns round-trip vs
Boost's 22.3 ns — 2.4×**. A register-pressure sweep (accumulators
held live across every switch) shows the win survives real liveness:
full-clobber stays at ~9 ns with 8 live values and beats Boost even
at 12 (18.4 vs 25.4). An in-between contract (asm saves a "commonly
live" subset, x19–x24; call site clobbers the rest) was also measured
and is **dominated at every pressure level** (13.6–19.6 ns): the
static subset pays unconditionally, while the clobber contract is
already the adaptive version of that idea — the compiler saves
exactly the live subset per call site, and its spill code schedules
better than the asm's serial store chain. **Shipped** (🎯T35.1): `make
LIGHT_SWITCH=1` selects it on arm64 macOS/Linux; Boost fcontext stays
the default and is always used on Windows, under sanitizers, and on
other architectures — the gate self-disables (verified: TSan suite
with the flag builds the Boost path, 739/739). The dist amalgamation
carries the assembly under the same gate. Realized, same epoch:
yield 71.3 → 49.1 ns (−31%); ping-pong 173.8 → 145.4 ns (−16%),
flat across 2–16 procs. The `alt_state` claim CAS stays seq_cst
deliberately (protocol margin over ~1 ns). TLS accessors at 1.5 ns
are no longer worth chasing — their earlier profile weight was call
frequency, already removed.

A bespoke count==1 prialt path was evaluated and **rejected**: with
the sort call gated and dedup trivially short, the n=1 flow is
protocol-minimal — its remaining cost is ~14 safety-required atomic
operations (slot spinlock, UAF pin/unpin, channel lock, alt_state
claim, suspend/placement words, run-queue insert), not generic-path
overhead.

**Measurement caveat.** The 16-proc fan-in/buffered shapes are
bimodal across measurement epochs (machine state; same binary varies
1.5–4.4 µs between sessions). Within-epoch A/B holds: FastMutex
improved fan-in ~4.4 → ~3.2 µs and buffered ~2.0 → ~1.7 µs alongside.
Cross-epoch comparisons of these shapes are unreliable; ping-pong and
yield are stable and comparable. 🎯T35's criteria are ratios within
one epoch for exactly this reason.

## Round 4 (2026-07-19): clearing the table

Every remaining identified opportunity evaluated; verdicts:

| Item | Verdict | Result |
|---|---|---|
| Worker QoS pinning (macOS) | **inefficacious — reverted** | A/B identical on all shapes; not the fan-shape bimodality driver |
| `do_switch(Status, Imp*)` overload | implemented | drops one TLS re-read per suspend |
| Channel-create trims | **implemented** | **156 → 61 ns**: creation did SEVEN allocations (channel + 2 slots + 4 eagerly-allocated waiter/vulture rings) plus an eager `descr_` snprintf+string; rings now allocate lazily on first use, the default "▸N" name formats on demand in `describe()` |
| Imp/Processor field layout | implemented, **neutral** in measured shapes | wake-protocol words grouped on one line, queue links on another, cold tail; `heartbeat` isolated. Kept: principled, free, benefits migration-heavy loads |
| ThinLTO (core TUs) | **marginal (~0–4%) — documented only** | the hot chain is already hand-merged; noinline TLS accessors correctly survive LTO; dist's single-TU model already gives users whole-program optimization |
| **Waker-side deregistration** | **implemented** | **ping-pong 168 → ~146 ns (−13%)**; buffered ~2.7 → ~2.3 µs. Every claim site (match, death ×2, swap ×2) removes a single-op sleeper's registration while holding that channel's `mu_`; the woken side skips its phase-3 relock entirely. `ChannelLifecycle.tla` rewritten (claimer-deregisters actions, new `NoDanglingRegistration` invariant — ring entries point into the sleeper's stack frame, so a missed removal is a UAF) |

Combined with the light switch: **ping-pong ~120 ns/op, flat at 2 and
16 procs** — 42× from the original 5,081 ns baseline, and the yield
round-trip stands at 49 ns against a 23 ns raw-jump floor.

At this point the per-rendezvous barrel is scraped: what remains in
the hot path is the raw switch (at its contract floor), one FastMutex
pair per side, the UAF pin/unpin, the alt_state claim, and the
suspend/placement words — each individually justified by a TLA-modeled
protocol. Further gains are architectural: 🎯T35's sticky registration
(fan-in is still ~3.2 µs at 16 procs vs 210 ns at 2) and O4
ring-buffer buffered channels (~2.3 µs vs ~50 ns potential).

## Postmortem: the release-gate CI reds (2026-07-19)

PR #95's first CI run failed three jobs; every failure traced to the
new scheduling dynamics exposing latent problems rather than protocol
bugs (all TLA-verified protocols held):

1. **Latent test race** — `Channel---NReaders` summed into a plain
   `int` from ten imps; wake-to-local made them truly concurrent
   (measured `total==63` of 1023). One-line atomic fix; a fanned-out
   audit of all ~75 by-ref spawn captures found every other site
   already hardened by the 🎯T29-era passes.
2. **Fake-clock orchestrator flake** — `Timer---MultipleTimersOrdering`
   ran its `alt` in the binding imp, which is deliberately NOT a
   quiescence-scope member, so fake time could advance past both
   deadlines before the alt registered — and `alt` picks randomly
   between two ready arms. The alt now runs in a spawned participant.
3. **TSan deadlock detector** — its per-OS-thread 64-entry held-lock
   table is incompatible with M:N fiber runtimes; overflow wedges the
   process (`CHECK failed: sanitizer_deadlock_detector.h`). Disabled
   in CI (`detect_deadlocks=0`); deadlock assurance comes from the
   TLA+ specs and the test watchdog.
4. **The real find: wake-to-local starvation compounding.** With one
   worker, `flat_map`'s merge and its outer producer monopolized the
   P's ring; 4,999 spawned sub-stream imps never left the global
   queue, so the input arm stayed the only ready one and the merge's
   vector alt grew by one channel per iteration (instrumented: 5,001
   arms after 5,000 consecutive input fires; each cycle paying O(N²)
   dedup + three O(N) lock passes). Fix: a **pull-based fairness
   budget** — every 32nd consecutive local wake pulls a
   `take_from_global` batch into the monopolized ring instead of
   spilling the woken peer (a spill design cost +170 ns/op at 16
   procs from pair migration). Measured after: 135–142 ns/op at both
   2 and 16 procs — no fairness tax — and the reproducer is gone.

Also surfaced, attributed, and deliberately not blocking: a
pre-existing ~4%-per-suite-run SIGABRT in network-suite teardown
(reproduced 1/25 on v0.22.0-era master) and a rare in-suite
`coin-flip---entropy` hang (2/45 branch, 0/25 master, 0/200
isolated — not statistically attributable). Tracked as 🎯T36.

## Round 5 (2026-07-19): 🎯T35 O4 Channel-owned ring buffer

Buffered `chan<T>(N)` no longer spawns a filter imp. The Channel
owns a type-erased ring (`BufOps` vtable for relocate_in/out/destroy)
and phase-1 of `prialt_begin_impl` services free-slot writes and
non-empty reads under `mu_` alone — TLA: `formal/BufferedChanRing.tla`
(+ `_Bug` no-wake counterexample). FIFO is preserved: readers drain
the buffer before taking from waiting writers; a free slot after pop
immediately parks one waiting writer's value.

| Configuration | before (filter imp) | after (O4 ring) |
|---|---:|---:|
| buffered `chan<int>(1024)`, 2 procs | ~500–1700 ns | **~45 ns** |
| buffered `chan<int>(1024)`, 16 procs | ~1670–2570 ns | **~45–60 ns** |

Acceptance criterion 2 of 🎯T35 (under 500 ns/element at 16 procs) is
met with margin. Oracles: 757/757 native, 739/739 TSan; TLC green on
BufferedChanRing / OptimisticAlt (+ failing _Bug counterparts).

**Remaining (🎯T35 criterion 1):** multi-writer `alt/8ch` still degrades
at high proc counts (~300 ns @ 2 vs ~3 µs @ 16). Sticky multi-op
registration was prototyped then reverted (lost-wakeup / dangling
ChanOp* hazards across alt() returns); TLA scaffold lives in
`formal/OptimisticAlt.tla`. Tracked as remaining parent acceptance.

## Method note

`bench/channel.bench.cc` had bit-rotted (`csp_chanop`, a removed C-API
type) — fixed in the commit accompanying this paper, so `make bench`
runs again. The numbers above came from focused drivers rather than
nanobench because per-op attribution needed `sample`-able steady-state
runs (macOS `sample`, no sudo — see paper 32's method note).
