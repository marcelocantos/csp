# The Engineering of CSP

A series of technical papers on the design, implementation, and
debugging of the CSP imp-based concurrency library.

## Papers

1. **[When Context Switching Breaks Your Compiler](01-tls-caching.md)**
2. **[Channels That Know When to Die](02-channel-lifecycle.md)**
3. **[Zero-Overhead Channel Synchronization](03-two-phase-prialt.md)**
4. **[Verifying a Imp Scheduler with TLA+](04-tla-verification.md)**
5. **[A Million Threads on a Megabyte](05-stack-engineering.md)**
6. **[Dynamic Scoping for Imps](06-dynamic-scoping.md)**
7. **[Topology Surgery on Live Channels](07-channel-fuse-split.md)**
8. **[Context-Aware Stack Depth Analysis](08-context-aware-stack-analysis.md)**
9. **[The Spawn HAMT Race and Catch-Block Migration Bug](09-spawn-hamt-race.md)**
10. **[The Teddy Bear Paper: diagnosing a C++ exception ABI race through structured articulation](10-teddy-bear-paper.md)**
11. **[The Channel Re-Resolution Use-After-Free](11-channel-reresolution-uaf.md)**
12. **[Verification Architecture for CSP Programs](12-verification-architecture.md)**
13. **[Formal Foundations of CSP Verification](13-formal-foundations.md)**
14. **[The Mutex-to-Channel Transformation: Porting a Go Broker to CSP](14-mutex-to-channel.md)**
15. **[Channels as Interfaces](15-channels-as-interfaces.md)**
16. **[TSan Cannot Track Mutex Ownership Across Fiber Migrations](16-tsan-fiber-mutex-interaction.md)**
17. **[net::listen Accept Loop Lifecycle — Investigation](17-net-listen-lifecycle.md)**
18. **[Dynamic Scope Alternatives to HAMT](18-dynamic-scope-alternatives.md)**
19. **[Quiescence Scope Gap: Analysis and Proposed Fix](18-quiescence-gap.md)**
20. **[Pull-Based Sources: Composable Sized Reads for CSP](19-pull-based-sources.md)**
21. **[Paper 20: Arena-Based Stack Scaling for 100K+ Imps](20-arena-stack-scaling.md)**
22. **[Paper 20: HTTP/3 over QUIC — Design and Integration Contract](20-http3-design.md)**
23. **[22. Main-loop busy-spin when quiescent without hook](22-main-loop-busy-spin.md)**
24. **[23. Stack Analysis Gap Audit (🎯T3.4 scoping)](23-stack-analysis-gaps.md)**
25. **[24 — Signal Handling Audit: Async-Signal-Safety](24-signal-handling-audit.md)**
26. **[Paper 24: Stack Pool Reclamation Race](25-stack-pool-reclamation.md)**
27. **[Paper 25: M:N Worker Join — Stability Loop and Watchdog Race](26-mn-worker-join.md)**
28. **[Paper 27 — `web_crawler` Linux-only deadlock](27-web-crawler-linux-hang.md)**
29. **[Streaming an HTTP Body Without Deadlocking the Connection](28-streaming-http-body-handoff.md)**
30. **[Paper 29: `tls::conn` over `tls::stream` — Wire-Synchronous Write *(design + implemented)*](29-tls-conn-over-stream.md)**
31. **[30. Walker Register Provenance Across BL Boundaries (🎯T3.10) *(design + implemented)*](30-walker-register-provenance.md)**
32. **[Paper 31 — tls::stream close_notify shutdown hang](31-buffered-chan-close-notify-hang.md)**
33. **[Paper 32 — Linux arm64 dist-TSan CI hang: analytic scan](32-arm64-tsan-hang-analysis.md)**
34. **[33 — Channel hot-path performance analysis](33-channel-hot-path-performance.md)**
35. **[Paper 34 — Suite-context SIGABRT and coin-flip hang](34-suite-teardown-abort-and-hang.md)**
36. **[35 — Non-channel performance surfaces (🎯T38)](35-non-channel-performance-surfaces.md)**
