# Formal Foundations of CSP Verification

## Abstract

CSP's concurrency model — move-only channel endpoints, synchronous
rendezvous, and bidirectional death propagation — is designed to make
certain classes of concurrency bugs structurally impossible. This
paper makes that claim precise. We define a minimal axiom system
(four axioms) for CSP's ownership and communication model, derive
three global properties (race freedom, orphan-block freedom, cleanup
completeness) from local per-imp checks, and show that these
derivations are compositional: each imp can be verified in isolation,
and the results compose into system-wide guarantees without reasoning
about interleavings. We compare this approach to Go's goroutine model
and Rust's ownership model, identifying where CSP's guarantees are
stronger, weaker, or orthogonal.

## 1. Motivation

Concurrent programs are hard to verify because the number of
possible interleavings grows combinatorially with the number of
threads and operations. A system with N threads each performing M
operations has on the order of (N·M)! / (M!)^N possible
interleavings. Even small systems produce state spaces that exhaust
model checkers.

The traditional response is to use synchronisation primitives
(mutexes, semaphores, barriers) and reason about which interleavings
are possible under the synchronisation discipline. This works, but
the reasoning is global: to prove that a mutex protects a variable,
you must verify that *every* access to that variable, across *every*
thread, holds the mutex. A single missed lock acquisition — possibly
in code added months later by a different developer — invalidates
the proof.

CSP takes a different approach. Instead of restricting which
interleavings are valid, it restricts how state is shared: not at
all. Each imp owns its state exclusively. Inter-imp communication
happens only through typed, synchronous channels with move-only
endpoints. This shifts the proof obligation from "verify all
interleavings" to "verify all ownership" — a fundamentally different
and more tractable problem.

This paper formalises that intuition.

## 2. The axiom system

We model a CSP program as a set of **imps** (lightweight
coroutines), a set of **channels**, and a set of **endpoints**
(typed, directional handles to channels). Each endpoint is either a
`writer<T>` or a `reader<T>`.

### Axiom A1: Linearity

> Each endpoint has exactly one owning imp at any point in time.

An endpoint can be **moved** (transferring ownership to another imp)
or **copied** (creating a new reference-counted alias via `.copy()`).
In either case, each alias is independently owned by exactly one
imp. The channel tracks the total number of live writer aliases and
reader aliases.

This axiom is enforced by C++ move semantics: `writer<T>` and
`reader<T>` have deleted copy constructors. The `.copy()` method
creates a new alias with shared ownership of the underlying channel,
but each alias object is itself move-only and owned by one imp.

**Note on enforcement.** C++ cannot fully prevent aliasing of
*values sent through* channels. A programmer can send a raw pointer
through a `chan<int*>` and access the pointed-to memory from both
sides. A1 guarantees ownership of the *endpoints*, not of arbitrary
memory. The race-freedom guarantee (§3.1) holds for programs that
follow the discipline: values cross channels, references do not.
Rust's borrow checker enforces this at the language level; C++
relies on convention. Section 5 discusses this difference.

### Axiom A2: Rendezvous atomicity

> A send on `writer<T>` and a receive on `reader<T>` for the same
> channel constitute a single atomic event. No intermediate state is
> observable by any imp.

For unbuffered channels, this is synchronous rendezvous: the sender
blocks until a receiver is ready, and the value transfer happens in
a single step. For buffered channels, the send and receive are
decoupled by the buffer imp, but each individual send-to-buffer and
buffer-to-receive is itself atomic.

This axiom is implemented by the two-phase prialt protocol (Paper 3),
which uses lock-free CAS operations to ensure that exactly one
sender and one receiver commit to each rendezvous.

### Axiom A3: Death propagation

> When all endpoints of one polarity (all writers or all readers) for
> a channel are dropped, all endpoints of the opposite polarity
> receive a **death signal**.

Specifically:

- When the last `writer<T>` for a channel is dropped (or the owning
  imp exits), every `reader<T>` for that channel transitions to a
  **dead-writer** state (EOF). Subsequent reads return false.

- When the last `reader<T>` for a channel is dropped, every
  `writer<T>` transitions to a **dead-reader** state (broken pipe).
  Subsequent writes return false.

Death is permanent and irrevocable. A dead channel cannot be
revived. This is the foundation of CSP's cleanup model (Paper 2).

### Axiom A4: Death observability

> Death signals are observable as first-class events in `alt` and
> `prialt` via **vultures** (`~endpoint`). A `prialt` that includes
> a vulture for endpoint `e` will return a complemented index when
> `e`'s channel dies, rather than blocking indefinitely.

Concretely:

```cpp
int n;
switch (prialt(r >> n, ~w)) {
    case  0: /* data received from r */  break;
    case ~1: /* w's reader died */       break;
}
```

The vulture `~w` fires when all readers of `w`'s channel are
dropped. The `prialt` returns the complement of the vulture's index
(`~1`), distinguishing death events from data events.

Without A4, death (A3) would still occur, but imps could not
*react* to it in the same control flow that handles data. They would
need to check for death after every operation — the Go `v, ok :=
<-ch` pattern. A4 makes death a peer of data in the multiplexing
model.

## 3. Derived properties

From these four axioms, three global properties follow. Each is
derived from a **local** check — a property of individual imps that
can be verified in isolation.

### 3.1 Race freedom

**Theorem.** If every piece of mutable state is owned by exactly one
imp and all inter-imp communication uses channels with atomic
rendezvous, then no data race exists.

**Proof.** A data race requires two conditions: (1) two threads
access the same memory location concurrently, (2) at least one
access is a write, and (3) the accesses are not ordered by
synchronisation.

By A1, each channel endpoint is owned by exactly one imp. Therefore
no two imps can concurrently access the same endpoint.

By A2, the value transferred through a channel changes ownership
atomically. The sender's write and the receiver's read are a single
event — they cannot be concurrent.

For state owned by an imp (local variables, data structures), only
that imp can access it. No other imp has a reference.

Therefore conditions (1) and (3) are never simultaneously satisfied.
No data race exists. ∎

**Local check.** For each imp: does this imp share any mutable state
with another imp outside of channel operations? If no imp does, the
global property holds. This is a per-imp inspection with no
interleaving reasoning.

**Caveat.** As noted in A1, this proof applies to channel endpoints
and imp-local state. If an imp sends a raw pointer through a channel
and both sides access the pointed-to memory, A1 is violated and the
proof does not apply. The discipline is: send values, not
references.

### 3.2 Orphan-block freedom

**Definition.** An imp is **death-covered** if, for every channel
endpoint `e` it waits on in a `prialt`, its `prialt` also includes a
vulture `~e` (or a vulture for a related endpoint whose death
implies `e`'s death).

**Theorem.** If every imp is death-covered, then no imp can block
forever due to a counterpart exiting.

**Proof.** Suppose imp A blocks in a `prialt` that includes channel
`C`. There are two cases:

1. C's counterpart is alive. Then A may eventually rendezvous (not
   an orphan block) or may deadlock with live imps (a logical
   deadlock, not an orphan block — see §4.1).

2. C's counterpart exits (all endpoints of the opposite polarity are
   dropped). By A3, a death signal is sent to C. By A4, A's vulture
   for C fires, and A's `prialt` returns with the complemented
   index. A unblocks.

In neither case does A block forever due to a counterpart exiting. ∎

**Local check.** For each imp: does every `prialt` include vultures
for all channels it waits on? This is a structural check on the
imp's source code — enumerate the `prialt` calls, verify that each
channel argument has a corresponding vulture.

### 3.3 Cleanup completeness

**Theorem.** If the imp dependency graph (nodes = imps, edges =
channels) is acyclic, then dropping the root imp's channel endpoints
causes all imps in the graph to terminate and release their
resources.

**Proof.** By induction on the depth of the DAG.

**Base case.** Leaf imps have no outgoing channel endpoints (no imps
depend on them). When their input channels die (A3), they observe
death (A4), exit their main loop, and drop their own endpoints. All
local resources are released by RAII.

**Inductive step.** Assume all imps at depth ≤ k terminate when
their inputs die. An imp at depth k+1 holds channel endpoints
connected to imps at depth ≤ k. When the root's endpoints are
dropped, death cascades to depth 1 (the root's immediate dependents),
then to depth 2, and so on. By the inductive hypothesis, imps at
each depth terminate, dropping their endpoints, which cascades to
the next depth. Eventually depth k+1 is reached, and those imps
terminate as well.

All imps terminate. All resources are released. ∎

**Graph check.** Verify that the process dependency graph is
acyclic. This is a standard graph traversal (topological sort or
cycle detection) with no interleaving reasoning.

**Note on cycles.** Cyclic dependency graphs do not satisfy this
theorem. Cycles are not inherently wrong — buffered channels and
`prialt` with timeouts can break cycles — but cleanup completeness
is not guaranteed by the structural argument alone. Cyclic
topologies require additional reasoning (e.g., proving that at least
one imp in the cycle will observe death or timeout independently of
the cycle).

## 4. What the axioms do not cover

### 4.1 Logical deadlocks

Two live imps can form a circular wait: A blocks sending to B while
B blocks sending to A. Both imps are alive, so A3 does not fire.
Both imps are blocked, so no vulture fires. This is a logical
deadlock — a bug in the program's protocol, not in the concurrency
primitives.

The axiom system does not prevent logical deadlocks. Detecting them
requires either:

- **Static analysis**: cycle detection in the channel dependency
  graph (Paper 12, §4.2).
- **Runtime monitoring**: a wait-for graph maintained by a monitor
  imp (Paper 12, §5).
- **Model checking**: TLA+ or FDR verification of the protocol
  (Paper 12, §8).

### 4.2 Protocol conformance

A `chan<int>` enforces that integers flow through the channel, but
nothing prevents sending values in the wrong order, sending too
many, or sending too few. The axioms cover ownership and lifecycle,
not protocol sequences.

Session types (Paper 12, §2) address this by encoding the expected
protocol into the type system, so that protocol violations become
compile-time errors.

### 4.3 Starvation

`prialt` with priority ordering deliberately starves lower-priority
channels. Fair `alt` mitigates this, but application-level
starvation (one producer dominating a shared channel) is not
detectable from the axioms.

Progress monitoring (Paper 12, §6) addresses this with heuristic
runtime checks.

### 4.4 Functional correctness

The axioms guarantee that values are transferred safely, but not
that the *right* values are transferred. Functional correctness —
"the system computes the right answer" — requires application-level
reasoning that no concurrency framework can automate.

## 5. Comparison with other models

### 5.1 Go: goroutines and channels

Go provides goroutines (lightweight threads) and channels, but does
not enforce exclusive ownership. The critical difference:

| Property | Go | CSP |
|----------|----|----|
| Channel ownership | Shared — any goroutine can hold a reference | Move-only — one owner per endpoint |
| Shared memory | Allowed alongside channels | Disallowed by convention (not by type system) |
| Channel closure | `close(ch)` from sender side only; panic on double close | Per-endpoint lifecycle; drop when done |
| Death detection | `v, ok := <-ch` — check after receive | Vultures in `alt`/`prialt` — observe as event |
| Cancellation | Explicit `context.Context` parameter threading | Dynamic scoping via `cancellation()` |

The consequence for verification:

**Race freedom.** Go's `go func() { shared++ }()` is legal — shared
memory coexists with channels. Go's race detector (`-race` flag)
exists precisely because the language does not prevent data races by
construction. CSP prevents them by construction (A1 + A2), subject
to the raw-pointer caveat.

**Orphan-block freedom.** In Go, if a goroutine sends to a channel
whose receiver has exited, the sender blocks forever. There is no
built-in mechanism to observe the receiver's death in a `select`.
The standard workaround is `context.Context` with explicit
cancellation — effective but requires manual threading of the
context through every function signature. In CSP, vultures (A4) make
death observable without any parameter threading.

**Cleanup completeness.** Go has no equivalent of death-cascade
cleanup. Goroutines are never forcibly terminated. A goroutine that
blocks on a dead channel leaks forever. The standard workaround is
`defer cancel()` on a context, but this only works if every
goroutine in the tree checks the context — a global coordination
requirement that CSP's structural model avoids.

### 5.2 Rust: ownership and Send/Sync

Rust's ownership model is the closest analogue to CSP's approach.
Both use linear/affine types to prevent aliasing of mutable state.
The comparison:

| Property | Rust | CSP |
|----------|------|----|
| Ownership enforcement | Borrow checker (compile-time, sound) | Move semantics (compile-time, convention-dependent) |
| Shared immutable access | `&T` references (safe) | Not modeled — convention only |
| Mutation | `&mut T` — exclusive (enforced) | Imp-local — exclusive (by convention) |
| Channel types | `Sender<T>` / `Receiver<T>` (move-only) | `writer<T>` / `reader<T>` (move-only) |
| Death propagation | Drop semantics; no channel-level death events | Bidirectional death propagation (A3 + A4) |
| Concurrency safety | `Send` / `Sync` traits — compile-time thread safety | No equivalent — convention-dependent |

**Where Rust is stronger.** Rust's borrow checker *proves* A1 at the
language level. A Rust program that compiles cannot violate A1 —
there is no raw-pointer escape hatch in safe code. CSP relies on the
programmer to follow the discipline. A C++ programmer can
`reinterpret_cast` an endpoint or send a raw pointer through a
channel, and the axioms no longer apply.

**Where CSP is stronger.** Rust has no equivalent of death
propagation (A3) or death observability (A4). When a Rust `Sender`
is dropped, the `Receiver` gets `None` on the next `recv()` — but
this is a return value, not an event that can be multiplexed with
other operations in a `select!`. Rust's `select!` macro (from
tokio or crossbeam) can wait on multiple channels, but cannot
observe channel death as a first-class event alongside data.

This means Rust programs face the same orphan-block risk as Go
programs when a producer exits unexpectedly. The standard workaround
is cancellation tokens or `JoinHandle` checks — manual coordination
that CSP's axiom system makes unnecessary.

**The synthesis.** CSP provides the *liveness* dimension (death
propagation, cleanup cascading) that Rust's ownership model lacks.
Rust provides the *safety* dimension (compiler-enforced ownership)
that CSP's C++ implementation lacks. A hypothetical CSP library
implemented in Rust would have both — compiler-enforced A1 plus
library-provided A3/A4 — giving the strongest guarantees of any
existing concurrency model.

### 5.3 Erlang/OTP: processes and monitors

Erlang's actor model shares CSP's "no shared state" philosophy but
takes a different approach to death:

| Property | Erlang | CSP |
|----------|--------|----|
| Death notification | Monitors and links — explicit opt-in | Vultures — per-endpoint, composable in alt/prialt |
| Restart policy | Supervisors — declarative restart trees | Not built-in (available via worker groups) |
| Channel model | Mailboxes — async, untyped | Channels — sync (default), typed |
| Selective receive | Pattern matching on mailbox | `alt`/`prialt` on channel endpoints |

Erlang's monitor/link system is the closest analogue to CSP's death
propagation. The key difference is granularity: Erlang monitors
observe *process* death, while CSP vultures observe *endpoint*
death. An imp can hold multiple channel endpoints and selectively
observe death on each one independently. An Erlang process is either
alive or dead as a unit.

This finer granularity makes CSP's cleanup more precise: an imp can
shut down one communication pathway (by dropping an endpoint) while
keeping others active. In Erlang, a crashed process takes all its
mailboxes with it.

## 6. Local verification and composition

The central claim of this paper is that verification under the CSP
axiom system is **compositional** — properties of the whole system
follow from properties of individual imps, checked in isolation.

### 6.1 The per-imp checklist

For each imp, three questions:

1. **Ownership.** Does this imp share any mutable state with another
   imp outside of channel operations?

   If no → contributes to global race freedom (§3.1).

2. **Death coverage.** Does every `prialt` in this imp include
   vultures for all channels it waits on?

   If yes → contributes to global orphan-block freedom (§3.2).

3. **Termination.** Does this imp exit (dropping all its endpoints)
   when its input channels die?

   If yes → contributes to global cleanup completeness (§3.3),
   assuming acyclic topology.

Each question is answered by inspecting the imp's code in isolation.
No reasoning about other imps' behaviour or about interleavings is
required.

### 6.2 Why this composes

In mutex-based concurrency, proving that a mutex protects a variable
requires a *global* argument: every access to that variable, across
every thread, must hold the mutex. Adding a new thread that accesses
the variable invalidates the proof unless the new thread also holds
the mutex. The proof obligation grows with the system.

In CSP, adding a new imp to the system requires checking only the
new imp against the three questions. Existing imps' proofs are
unaffected because they share no state with the new imp (A1). The
proof obligation is constant per imp, regardless of system size.

This is the same insight that makes Rust's ownership model powerful:
local reasoning composes into global guarantees. CSP extends this
from data races (Rust's strength) to liveness properties (death
propagation, cleanup cascading) that ownership alone does not cover.

### 6.3 The interleaving-free zone

Traditional concurrent verification must consider all interleavings
of operations across threads. CSP restricts communication to
synchronous rendezvous (A2), which has a simple semantics: a
rendezvous is a single event in the process algebra, not a pair of
interleaved operations. The number of events grows linearly with the
number of rendezvous, not combinatorially with the number of
possible interleavings.

This does not eliminate all complexity — the *order* in which
rendezvous occur still matters for protocol correctness (§4.2) and
deadlock analysis (§4.1). But it eliminates the combinatorial
explosion of fine-grained memory interleavings that makes mutex-based
verification intractable.

## 7. Implications for tooling

The axiom system and compositionality results directly inform the
verification tooling architecture proposed in Paper 12:

- **Session types** (Paper 12, §2) operationalise the protocol gap
  (§4.2) by encoding the expected communication sequence into the
  type system. They do not follow from the axioms — they extend
  them.

- **Graph validation** (Paper 12, §4) operationalises the topology
  checks: acyclicity (§3.3), death coverage (§3.2), and
  connectivity.

- **Deadlock detection** (Paper 12, §5) addresses the logical
  deadlock gap (§4.1) at runtime, since the axioms cannot prevent it
  statically.

- **Model extraction** (Paper 12, §8) allows verification of
  properties beyond the axiom system's scope — arbitrary safety and
  liveness invariants — by translating CSP programs into established
  model-checking frameworks (FDR, TLA+).

The axiom system tells us *what* can be verified locally and *what*
requires additional machinery. The tooling provides that machinery.

## 8. Toward a proof assistant formalisation

The axiom system presented here is semi-formal — the proofs are
structured arguments, not machine-checked derivations. A natural
next step is formalisation in a proof assistant (Coq, Lean, Agda).

The key challenges:

1. **Modeling C++ semantics.** The axioms assume move-only semantics,
   but C++ has escape hatches (raw pointers, `reinterpret_cast`,
   `const_cast`). A formalisation would need to either model a
   "safe subset" of C++ or add A1 as an axiom (trusted assumption).

2. **Modeling the scheduler.** The M:N scheduler multiplexes imps
   across OS threads. The axioms abstract over the scheduler, but a
   full formalisation would need to prove that the scheduler
   preserves the axioms — particularly A2 (rendezvous atomicity).
   Paper 4's TLA+ specs partially address this.

3. **Modeling buffered channels.** Buffered channels interpose a
   buffer imp between sender and receiver. A2 applies to each
   individual transfer (sender→buffer, buffer→receiver), but the
   end-to-end semantics are asynchronous. The formalisation must
   handle this composition.

4. **Modeling dynamic topology.** Channels can be created and
   destroyed at runtime, and channel endpoints can be sent through
   other channels (channels-of-channels). The axioms handle this —
   A1 applies regardless of when or how an endpoint is created — but
   the formalisation must represent a dynamically evolving graph.

These are tractable but non-trivial. The semi-formal presentation in
this paper is intended as a stepping stone toward full
mechanisation.
