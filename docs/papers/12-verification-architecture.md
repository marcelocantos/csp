# Verification Architecture for CSP Programs

## Abstract

CSP's channel-based concurrency model — move-only endpoints,
synchronous rendezvous, and bidirectional death propagation — makes
certain classes of concurrency bugs structurally impossible. But
"impossible by convention" is not the same as "impossible by proof."
This paper proposes a layered verification architecture that
validates CSP program correctness at compile time, wire-up time, and
execution time, turning informal reasoning about channel ownership
and protocol conformance into mechanically checked invariants. The
key insight is that CSP's locality of ownership makes verification
compositional: each imp can be checked independently, and the
results compose into global guarantees without reasoning about
interleavings.

## 1. What CSP already guarantees

Before designing new tooling, it is worth being precise about what
the current library provides and where the gaps are.

### 1.1 Structural guarantees (by construction)

**No data races on channel-mediated state.** If all shared mutable
state is owned by a single imp and accessed only through channels,
data races on that state are impossible. The argument is
straightforward: a data race requires concurrent unsynchronised
access to the same memory location. Channel rendezvous is atomic
(Paper 3). Move-only endpoints prevent aliasing of channel handles.
Therefore no two imps can concurrently access the same channel
endpoint, and the value transferred through a channel changes
ownership atomically.

This guarantee is structural but not enforced by the type system. A
programmer can pass a raw pointer through a channel and access it
from both sides. C++ cannot prevent this the way Rust's borrow
checker can. The guarantee holds for programs that follow the
discipline.

**Endpoint lifecycle.** When all writers for a channel are dropped,
all readers observe EOF. When all readers are dropped, all writers
observe a broken pipe. This is bidirectional death propagation
(Paper 2). Combined with vultures in `alt`/`prialt`, it means any
imp waiting on a channel whose counterpart has exited will be
notified rather than blocking forever.

**Cleanup cascading.** If the imp dependency graph is acyclic,
dropping the root imp's channel endpoints cascades death through the
entire graph. Each imp observes death via vultures, exits, drops its
own endpoints, and propagates further. By induction on graph depth,
all imps terminate and all resources are freed.

### 1.2 What is not guaranteed

**No logical deadlocks.** Two live imps can still form a circular
wait: A blocks sending to B while B blocks sending to A. Death
propagation does not help here — both imps are alive.

**No protocol violations.** A `chan<int>` enforces that integers
flow through the channel, but nothing prevents sending values in the
wrong order, sending too many, or sending too few. The protocol
(sequence of operations) is unchecked.

**No topology errors.** Nothing prevents constructing a channel,
forgetting to connect one endpoint, and leaking the other into a
permanent block.

**No starvation.** `prialt` deliberately starves lower-priority
channels. Fair `alt` helps, but starvation in the application
protocol (e.g., one producer dominating a shared channel) is not
detected.

These gaps define the verification architecture's scope.

## 2. Compile-time: session types

Session types encode the expected communication protocol into the
type system. Instead of a channel that carries `int`, a session
channel carries `send<int>` then `recv<string>` then `end` — the
full protocol, checked at compile time.

### 2.1 Protocol definition

A protocol is a type-level description of a communication sequence:

```cpp
// Simple request/reply
using rpc = csp::protocol<
    send<Request>,
    recv<Response>
>;

// Worker protocol from a task broker
using worker = csp::protocol<
    recv<string>,                 // receive model tag
    choice<                       // broker decides:
        branch<task_tag,          //   task available
            send<bytes>,          //   send payload
            end                   //   done
        >,
        branch<timeout_tag,       //   no task within window
            end                   //   clean close
        >
    >
>;

// Unbounded request/reply loop
using echo = csp::protocol<
    recv<string>,
    send<string>,
    recurse                       // back to start
>;
```

### 2.2 Dual types

Every protocol has a **dual** — the mirror image obtained by
swapping every `send` with `recv` and every `choice` (internal
choice, offerer selects) with `offer` (external choice, peer
selects). If one end of a session channel follows protocol `P`, the
other end must follow `dual<P>`. This is computed at compile time
via template specialisation.

```cpp
auto [client, server] = csp::session<rpc>{};
// client : session_end<rpc>
// server : session_end<dual<rpc>>
```

### 2.3 Linear consumption

Each operation on a session endpoint **consumes** the endpoint and
produces a new one at the next protocol state. This is the session
type discipline: the current state of the protocol is encoded in the
C++ type, and advancing the protocol requires a value-level
operation that changes the type.

```cpp
void handle(session_end<dual<rpc>> s) {
    auto [req, s1] = s.recv();    // s consumed; s1 is at the next state
    auto resp = process(req);
    s1.send(resp);                // s1 consumed; protocol complete
    // Compile error if you try to use s or s1 again
    // Compile error if you return without completing the protocol
}
```

For branching protocols, `select` (offerer) and `match` (peer)
produce branch-specific continuation types:

```cpp
void broker_side(session_end<worker> s) {
    s.send(model_tag);
    if (have_task) {
        auto s1 = s.select<task_tag>();   // enter task branch
        auto [payload, s2] = s1.recv();   // branch-specific state
        s2.send(result);                  // complete
    } else {
        s.select<timeout_tag>();          // enter timeout branch; done
    }
}
```

### 2.4 What session types catch

- Sending when the protocol says receive (direction error).
- Sending a value of the wrong type.
- Forgetting to handle a branch in a choice.
- Using an endpoint after the protocol says it is complete.
- Two imps disagreeing about their shared protocol (dual mismatch at
  channel construction).

### 2.5 What session types do not catch

- **Dynamic topologies.** When channels are created at runtime (e.g.,
  channels-of-channels for pub/sub), session types apply to each
  individual channel but cannot enforce the meta-protocol of how
  channels are created and connected.
- **Value-level correctness.** Session types check that an `int` was
  sent, not that it was the *right* `int`.
- **Timing.** Session types say nothing about deadlines or progress.

### 2.6 Implementation considerations

C++20 concepts can enforce the type constraints. The main challenge
is error message quality — deeply nested template errors are
hostile. Mitigations:

- `static_assert` with descriptive strings at each protocol
  transition.
- A `protocol_error<Expected, Got>` type alias that produces a
  readable error.
- A companion `csp-check` tool that parses compiler errors and
  reformats them as protocol violation reports.

The template machinery is estimated at ~500–800 lines. Session types
are opt-in — existing `chan<T>` usage is unaffected.

## 3. Compile-time: arity constraints

Channel endpoints are move-only by default, but `.copy()` creates
reference-counted aliases. Sometimes the topology demands a specific
fan-in/fan-out structure:

```cpp
// Single-producer, single-consumer — .copy() deleted on both sides
auto [w, r] = csp::chan<int, csp::spsc>{};

// Multi-producer, single-consumer — .copy() on writer only
auto [w, r] = csp::chan<int, csp::mpsc>{};

// Single-producer, multi-consumer — .copy() on reader only
auto [w, r] = csp::chan<int, csp::spmc>{};
```

This is a template parameter that selectively `delete`s `.copy()`
on one or both endpoint types. It documents intent and catches
structural errors at compile time. Cost: ~20 lines of SFINAE or
concepts.

## 4. Wire-up time: graph validation

Between constructing a process graph and calling `schedule()`, the
graph can be validated for structural soundness. This layer operates
on a graph representation — either explicit (a `csp::graph` builder
object) or inferred (by instrumenting channel construction and
`spawn()` calls in a validation mode).

### 4.1 The graph builder

```cpp
auto g = csp::graph{};

auto producer  = g.process("producer",  ports::out<int>("items"));
auto filter    = g.process("filter",    ports::in<int>("items"),
                                        ports::out<int>("filtered"));
auto consumer  = g.process("consumer",  ports::in<int>("filtered"));

g.connect(producer["items"], filter["items"]);
g.connect(filter["filtered"], consumer["filtered"]);

auto report = g.validate();
// report.ok()          → true if no errors
// report.warnings()    → informational diagnostics
// report.errors()      → structural violations
```

### 4.2 Checks

**Unconnected ports.** An endpoint was declared but never connected.
The owning imp will block on first use. Error.

**Dangling endpoints.** An endpoint was created (via `chan<T>`) but
neither connected nor explicitly dropped. It will be destroyed when
it goes out of scope, but this is usually a mistake. Warning.

**Cycle detection.** A cycle in the process graph *may* deadlock.
Cycles involving buffered channels or `prialt` with timeouts are
often intentional, so this is a warning with context:

```
⚠ cycle: filter → consumer → filter (via feedback channel)
  → cycle contains buffered channel (cap=16): likely intentional
```

Cycles with only unbuffered, non-timeout channels are flagged as
errors.

**Missing death coverage.** For each process P and each channel C
that P reads from, check whether P's `prialt` includes a vulture
for C (`~C`). If not, P could orphan-block if C's writer dies. This
is the structural precondition for the orphan-block-freedom theorem
from §1.1.

This check requires behavioural annotations — the graph builder
alone cannot see inside the imp's function body. Options:

- **Annotation-based**: `g.process("filter", ..., death_covered("items"))`.
- **Convention-based**: If a process declares input port `X`, it
  is assumed to watch `~X`. Violation is caught at runtime (layer 6).
- **Static analysis**: A Clang AST plugin scans the lambda for
  `prialt` calls and matches vulture arguments against declared
  ports.

**Type compatibility.** When connecting two ports, verify that the
channel types match. With session types (§2), also verify that the
protocols are duals. For dynamically typed connections (using a
protocol registry), this is a runtime check at connection time.

### 4.3 Output format

The validation report is a structured object that can be printed,
serialised to JSON, or consumed programmatically:

```
graph validation:
  ✓ all ports connected (7/7)
  ✓ no type mismatches
  ✓ no dangling endpoints
  ⚠ cycle: filter → consumer → filter [buffered, cap=16]
  ✗ missing death coverage: consumer reads "filtered" with no ~filtered
  1 error, 1 warning
```

## 5. Runtime: deadlock detection

A monitor imp maintains a **wait-for graph** (WFG) from
instrumented `prialt` calls.

### 5.1 Instrumentation

When deadlock detection is enabled (`csp::enable_monitor(csp::monitor::deadlock)`),
each `prialt` entry and exit sends an event to the monitor:

```
imp A enters prialt → monitor records: A waits on {ch1, ch2}
imp B enters prialt → monitor records: B waits on {ch1}
A and B rendezvous  → monitor records: A and B no longer waiting
```

The monitor receives events on a dedicated channel and periodically
checks the WFG for cycles using Tarjan's algorithm.

### 5.2 Detection report

```
DEADLOCK detected at t=1.342s:
  imp "producer" (spawned at pipeline.cc:42)
    waiting to send on channel 0x7f...a0 ("items")
  imp "consumer" (spawned at pipeline.cc:58)
    waiting to send on channel 0x7f...b0 ("feedback")
  → producer holds reader of 0x7f...b0
  → consumer holds reader of 0x7f...a0
  → circular wait (2 imps, 2 channels)
```

### 5.3 Cost

One channel send per `prialt` entry/exit. The monitor itself is an
imp with a buffered input channel, so it does not add synchronisation
points to the application's critical path. In release builds, the
instrumentation compiles out (`#ifdef CSP_MONITOR`).

## 6. Runtime: progress monitoring

Detect stalls, starvation, and throughput anomalies.

### 6.1 Metrics

Each successful rendezvous increments a per-channel counter. The
progress monitor snapshots these counters on a `tick()` and checks:

- **Stalled channel**: counter unchanged for N intervals (default 3).
  An imp is waiting, but nothing is arriving.
- **Imbalanced fan-in**: one writer accounts for >90% of
  rendezvous on a multi-writer channel. Lower-priority writers in a
  `prialt` may be starved.
- **Throughput drop**: rendezvous rate dropped below 10% of the
  rolling average. Something changed.

### 6.2 Implementation

```cpp
void progress_monitor(reader<rendezvous_event> events) {
    std::map<channel_id, channel_stats> stats;
    for (auto t : tick(1s)) {
        rendezvous_event ev;
        while (prialt(events >> ev, csp::none) == 0)
            stats[ev.channel].record(ev);

        for (auto& [id, s] : stats) {
            if (s.stalled(3))     warn_stall(id, s);
            if (s.imbalanced())   warn_starvation(id, s);
            if (s.throughput_drop(0.1)) warn_anomaly(id, s);
        }
    }
}
```

The monitor is itself a CSP program — an imp reading from a channel,
using `prialt` with `tick()` for periodic checks, following the same
patterns it monitors. This is intentional: the monitor validates its
own assumptions about progress.

## 7. Runtime: causal tracing

Record the happens-before relation for post-mortem analysis.

### 7.1 Event model

Each rendezvous creates a causal edge:

```
sender.event[n] → receiver.event[m]
```

Each death propagation creates a causal edge:

```
dying_imp.exit → observer.vulture_fired
```

The trace is a **partial order** (a DAG), not a total order. This is
critical: it captures actual causal dependencies, not arbitrary
interleaving artifacts. Two events without a path between them are
concurrent — neither caused the other.

### 7.2 Output

```cpp
csp::enable_monitor(csp::monitor::trace);
// ... run system ...
csp::dump_trace("trace.json");   // Lamport-style event DAG
csp::dump_trace("trace.msc");    // message sequence chart
```

External tooling (web UI, VS Code extension) consumes the trace and
renders it as a message sequence chart or a space-time diagram.

The causal trace is complementary to the deadlock detector: the
detector tells you *that* a deadlock occurred; the trace tells you
*how* the system reached that state.

## 8. Offline: model extraction

The most ambitious layer. Extract a formal model from CSP program
source and verify it with an established model checker.

### 8.1 CSP_M extraction for FDR

The CSP library implements Hoare's CSP. The mapping to CSP_M (the
input language for the FDR model checker) is direct:

| C++ construct | CSP_M construct |
|---------------|-----------------|
| imp with `prialt` | process with external choice (□) |
| channel send/recv | event synchronisation |
| death propagation | termination (SKIP) |
| `.copy()` (shared endpoint) | parallel composition with shared alphabet |
| `spawn()` | interleaving (|||) |
| buffered channel | buffer process (FIFO) |

A Clang AST plugin or annotation-driven extractor walks the source
and produces a CSP_M model:

```csp
-- Auto-extracted from pool_imp()
channel register, request, reply
channel shutdown

POOL = (register?tw -> POOL_DISPATCH(tw))
     [] (request?dr -> POOL_WAIT(dr))
     [] (shutdown -> SKIP)

POOL_DISPATCH(tw) =
    (request?dr -> if match(tw, dr)
                   then reply!tw -> POOL
                   else POOL_DISPATCH(tw))
    [] (register?tw2 -> POOL_MULTI({tw, tw2}))

-- Automatically checked:
assert SYSTEM :[deadlock free]
assert SYSTEM :[divergence free]
```

### 8.2 TLA+ extraction

For systems with richer state (queues with timeouts, retry logic,
resource pools), TLA+ is more natural. The CSP library's formal/
directory already contains TLA+ specs for internal protocols
(Paper 4). The same approach extends to application-level
verification:

```tla
---- MODULE PoolSpec ----
VARIABLES workers, waiters

TypeOK ==
    /\ workers \in SUBSET Worker
    /\ waiters \in Seq(Waiter)

NoWorkerInBothPools ==
    \A w \in workers: \A d \in Range(waiters): w.id /= d.worker_id

Liveness ==
    \A d \in Range(waiters):
        <>(d.state = "replied" \/ d.state = "timed_out")
====
```

### 8.3 Practical approach: annotated extraction

Full automatic extraction from C++ source to formal model is
research-grade work. A pragmatic path:

1. **Annotations** declare the intended invariants and protocol
   structure in the source.
2. An extractor generates a **model skeleton** from the annotations
   and function signatures.
3. The developer fills in the **state mapping** — which C++ variables
   correspond to which TLA+ variables.
4. CI runs TLC on every commit, checking the model against its
   invariants.

The annotations serve double duty: they are documentation (readable
by humans) and specification (checkable by machines).

```cpp
// [[csp::model("Pool")]]
// [[csp::invariant("no worker simultaneously pooled and dispatched")]]
// [[csp::liveness("every request eventually replied or timed out")]]
void pool_imp(reader<tagged_worker> registrations,
              reader<dispatch_req> requests,
              reader<> shutdown) {
    // ...
}
```

## 9. The compositionality argument

The central claim of this paper is that CSP's ownership model makes
verification **compositional** — local properties of individual imps
compose into global properties of the system without reasoning about
interleavings.

The argument proceeds in three steps.

### 9.1 Local ownership implies global race-freedom

If every piece of mutable state is owned by exactly one imp at any
point in time, and all inter-imp communication goes through channels
with atomic rendezvous, then there are no data races. This is a
global property that follows from a local one (single ownership per
imp). Checking it requires inspecting each imp in isolation — does
this imp share any mutable state outside of channels? — and
composing the results.

### 9.2 Death coverage implies orphan-block freedom

Define an imp as **death-covered** if, for every channel it waits
on in a `prialt`, it also includes a vulture for that channel. If
every imp is death-covered, then no imp can block forever due to a
counterpart exiting. The proof: if imp A blocks on channel C, and
C's counterpart exits, A's vulture for C fires, and A unblocks.
Again, a global property (no orphan blocks) from a local check
(each imp's `prialt` includes appropriate vultures).

### 9.3 Acyclic topology implies cleanup completeness

If the process dependency graph (edges = channels between imps) is
acyclic, then dropping the root's endpoints cascades death through
the entire graph. By induction on depth: leaf imps have no outgoing
channels; they observe death on their inputs and exit. Their exit
propagates to their parents, and so on up to the root. This is a
topological property checkable by graph traversal — no interleaving
reasoning required.

### 9.4 Composition

These three properties compose:

- Race-freedom + orphan-block freedom + cleanup completeness =
  **a system that cannot race, cannot orphan-block, and will
  cleanly shut down when the root exits**.

Each property is checked by a different mechanism (type-level for
races, graph-level for topology, annotation-level for death
coverage), but they combine without interaction effects because
they operate on orthogonal aspects of the system.

This is the sense in which verification is "trivial" — not that the
tools are trivial to build, but that the proofs are trivial to
construct once the tools exist. Each proof obligation is local and
independent. There is no combinatorial explosion of interleavings to
consider.

## 10. Layered summary

| Layer | When | What it checks | Guarantee | Cost |
|-------|------|----------------|-----------|------|
| Session types | Compile | Protocol conformance | Proven | Medium |
| Arity constraints | Compile | Fan-in/fan-out structure | Proven | Low |
| Graph validation | Wire-up | Connectivity, death coverage, cycles | Structural | Medium |
| Protocol composition | Wire-up | Dynamic endpoint compatibility | Structural | Low |
| Deadlock detection | Runtime | Circular waits | Definitive | Medium |
| Progress monitoring | Runtime | Stalls, starvation | Heuristic | Low |
| Causal tracing | Post-mortem | Root cause analysis | Observational | Medium |
| Model extraction | CI/offline | Deadlock, liveness, refinement | Proven (model) | High |

The recommended starting point is **session types + graph validation
+ deadlock detection**. Session types catch the largest class of
bugs (protocol violations) at the cheapest time (compile). Graph
validation catches wiring mistakes before execution. The deadlock
detector is the safety net for anything static analysis misses.

## 11. Relationship to existing work

Paper 2 (channel lifecycle) established the theory of bidirectional
death propagation. Paper 4 (TLA+ verification) demonstrated formal
verification of internal scheduler protocols. This paper extends
both: it uses death propagation as the foundation for compositional
reasoning (§9), and it proposes extending the TLA+ methodology from
internal protocols to application-level verification (§8).

The session types proposal draws on the linear logic tradition
(Wadler 2012, Caires and Pfenning 2010) adapted to C++ via
move semantics. The key insight is that C++'s move-only types
provide *affine* typing (use at most once), which is sufficient for
session type discipline — a consumed endpoint cannot be reused.

The graph validation layer is related to the static analysis work on
Go's channel operations (Stadtmüller et al. 2016), but benefits from
CSP's stronger ownership model: in Go, a channel can be shared by
arbitrary goroutines without restriction, making static analysis
fundamentally harder.

## 12. Open questions

**Can session types express recursive protocols with data-dependent
branching?** The `recurse` combinator handles simple loops, but
protocols where the number of iterations depends on a runtime value
(e.g., "send N items, then done") require dependent types that C++
cannot express. A runtime protocol checker (§6) may be the right
layer for these.

**How should the graph builder handle dynamic topologies?** Patterns
like channels-of-channels (pub/sub, actor spawning) create channels
at runtime. The graph builder can validate the initial topology, but
new channels created after `schedule()` bypass it. A runtime graph
monitor that maintains a live topology and checks new connections
against declared constraints is one approach.

**What is the right granularity for model extraction?** Extracting a
full model from a large program produces a state space too large for
TLC. Extracting individual imp protocols is tractable but misses
inter-protocol interactions — precisely the bugs that Paper 4 found
in the scheduler. The right granularity is likely a manually scoped
"interaction boundary" that captures 3–5 interacting imps and their
shared channels.

**Can the monitor infrastructure verify itself?** The deadlock
detector and progress monitor are themselves imps communicating via
channels. In principle, they could monitor each other. In practice,
this creates a bootstrap problem: who monitors the monitors? A
likely answer is that the monitors are simple enough to verify
statically (session types + model checking) and do not need runtime
self-monitoring.
