# The Engineering of CSP

A series of technical papers on the design, implementation, and
debugging of the CSP imp-based concurrency library.

## Papers

1. **[When Context Switching Breaks Your Compiler](01-tls-caching.md)**
   — How Clang's TLS address caching interacts with userspace context
   switching to produce crashes invisible to every standard debugging
   tool.

2. **[Channels That Know When to Die](02-channel-lifecycle.md)** —
   Per-endpoint lifecycle, death as a first-class event, and how the
   model enables 50+ composable stream combinators.

3. **[Zero-Overhead Channel Synchronization](03-two-phase-prialt.md)**
   — A two-phase protocol that eliminates indirect function calls from
   channel operations while maintaining type safety through a
   compilation firewall.

4. **[Verifying a Imp Scheduler with TLA+](04-tla-verification.md)**
   — Five TLA+ specifications that formally verify the scheduler's
   suspension, work-stealing, lifecycle, claiming, and parking
   protocols.

5. **[A Million Threads on a Megabyte](05-stack-engineering.md)** —
   Demand-paged virtual stacks, a recycling pool, and an ARM64
   instruction walker that estimates stack depth at spawn time.

6. **[Dynamic Scoping for Imps](06-dynamic-scoping.md)** — A
   persistent HAMT provides inherited, copy-on-write-isolated,
   channel-sendable per-imp variables.

7. **[Topology Surgery on Live Channels](07-channel-fuse-split.md)**
   — Fuse and split splice instrumentation, diagnostics, and
   replacement processes into live channel graphs without modifying
   application logic.

8. **[Context-Aware Stack Depth Analysis](08-context-aware-stack-analysis.md)**
   — Spawn-time analysis occupies a unique point between static analysis
   and profiling: it can read live runtime state to resolve indirect calls,
   prune unreachable paths, and calibrate budgets.

9. **[The Spawn HAMT Race](09-spawn-hamt-race.md)** *(stub)* — A
   use-after-free in the spawn warmup handshake freed a HAMT node
   before the child imp could retain it, plus an open investigation
   into a related exception-lifetime race in the supervisor restart
   path.

10. **[The Teddy Bear Paper](10-teddy-bear-paper.md)** — Diagnosing
    a C++ exception ABI race through structured articulation.

11. **[The Channel Re-Resolution Use-After-Free](11-channel-reresolution-uaf.md)**
    — A use-after-free triggered by channel swap topology changes,
    endpoint death, and the lock-free re-resolution window in the
    prialt retry path.

12. **[Verification Architecture for CSP Programs](12-verification-architecture.md)**
    — A layered verification framework — session types, graph
    validation, deadlock detection, and model extraction — that
    exploits CSP's ownership model to make concurrency proofs
    compositional.

13. **[Formal Foundations of CSP Verification](13-formal-foundations.md)**
    — Four axioms (linearity, rendezvous atomicity, death
    propagation, death observability) from which race freedom,
    orphan-block freedom, and cleanup completeness follow as
    local, compositional properties.

14. **[The Mutex-to-Channel Transformation](14-mutex-to-channel.md)**
    — Porting a Go task broker to CSP as a case study: every mutex
    becomes a channel-owning imp, shutdown reduces from 25 lines to
    one, and the total line count stays the same.

15. **[Channels as Interfaces](15-channels-as-interfaces.md)**
    — Bidirectional lifecycle observability as a design principle:
    how independent endpoint death signals enable a compositional
    architecture where components are wired via channel topology
    instead of called via APIs, and fuse/splice/swap become
    the composition operators.

16. **[TSan Cannot Track Mutex Ownership Across Fiber Migrations](16-tsan-fiber-mutex-interaction.md)**
    — Discovery and diagnosis of a TSan limitation: fiber annotations
    don't extend to pthread mutex tracking. When M:N-scheduled imps
    migrate between OS threads while holding mbedTLS mutexes, TSan
    reports false races. Root cause, investigation timeline, and
    implications for M:N schedulers.

17. **[net::listen Lifecycle and Shutdown](17-net-listen-lifecycle.md)**
    — Investigation of the net::listen accept loop lifecycle, stopper
    pattern, and fcontext terminate interaction with the M:N scheduler.

18. **[Dynamic Scope Alternatives to HAMT](18-dynamic-scope-alternatives.md)**
    — Exploration of alternative designs for dynamic scoping: chained
    stack arrays, segment lists, garbage collection with opaque handles,
    and flat root optimisation. All parked — the HAMT works and isn't a
    bottleneck.

19. **[Pull-Based Sources](19-pull-based-sources.md)** *(design)*
    — A `source = writer<request<size_t, bytes>>` abstraction that
    gives consumers size-control over reads while still composing as
    channels. Closes the push/pull gap at the `net::connection`
    boundary and the TLS composition gap; enables streaming HTTP
    bodies and a route out of `http::serve`'s direct-fd bypass.

21. **[Distribution Amalgamation for Protocol Implementations](21-dist-protocol-amalgamation.md)** *(design)*
    — Proposal for including protocol implementations (HTTP, HTTP/2, WebSocket,
    HTTP/3, QUIC) in the dist bundle. Compares all-in `csp.cpp` (option a)
    against a separate `csp_protocols.cpp` opt-in file (option b). Recommends
    option (b) to avoid inflating the core dist with ~93K lines of vendored C.
    Surfaces four open questions for the user before implementation begins.
