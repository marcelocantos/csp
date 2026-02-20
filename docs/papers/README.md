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
   — Fuse and split rewire channel graphs at runtime by composing
   slot swaps with temporary channels and RAII endpoint death.
