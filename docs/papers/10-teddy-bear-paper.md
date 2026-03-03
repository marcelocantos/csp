# The Teddy Bear Paper: diagnosing a C++ exception ABI race through structured articulation

## Setup

CSP is a C++ M:N concurrency library where lightweight "imps" (green
threads) are multiplexed across OS threads via work-stealing. A prior
session had fixed one bug (HAMT use-after-free in spawn) but a second
crash remained: SIGSEGV in `__cxa_end_catch` at address 0x0, ~1 in 10
runs, only under glibc's `MALLOC_PERTURB_=42` memory poisoning on
Linux. The crash was in the supervisor restart path — a worker imp
throwing an exception while being monitored by a supervisor.

Before ending that prior session, I asked Claude to write a paper stub
documenting the problem. The paper enumerated the actors (worker imp,
supervisor imp, killyou chain, stack pool), traced the exception
propagation sequence step by step, stated a hypothesis ("exception_ptr
references memory on the dying imp's stack"), and proposed a TLA+ spec
with a named invariant.

## The diagnosis

The new session restored context via `/pop` and set out to write TLA+
specs. But before writing specs, Claude needed to read the relevant
code to scope the state space. While reading `spawn_entry` — the exact
function the paper stub had traced through — the bug was immediately
visible:

```cpp
catch (...) {
    auto ex = std::current_exception();
    if (!(sd->w << ex) && ...) { std::terminate(); }
}
```

The channel send (`sd->w << ex`) is inside the catch block. In M:N
mode, a channel send can suspend the imp if no reader is ready. When
the imp resumes on a different OS thread, `__cxa_end_catch` runs on
thread B while `__cxa_begin_catch` registered the exception on thread
A's thread-local `__cxa_eh_globals`. Thread B's exception state is
empty → null pointer → SIGSEGV.

The fix was trivial: move the channel send outside the catch block.
`std::exception_ptr` is a refcounted handle — safe to use after the
catch ends.

## What's interesting

**The paper stub's hypothesis was wrong but its enumeration was
right.** The hypothesis said "exception_ptr references stack memory" —
incorrect, since `std::current_exception()` heap-allocates. But the
step-by-step trace ("catches → writes exception_ptr to channel") named
the exact code where the bug lived. Reading that trace with fresh eyes
made the yield-inside-catch immediately visible.

**The bug was found during scoping work for TLA+, not by TLA+
itself.** The plan was: read code → write spec → run model checker →
find interleaving. The reading phase was supposed to be preparation,
but it turned out to be sufficient. TLA+ specs were written afterward
as documentation and regression guards (the buggy spec finds the
violation in 5 states).

**A second instance of the same pattern was found** in `try_map.h` — a
stream combinator that sent exceptions on an error channel inside a
catch block. Same fix applied.

## The process insight

The user observed that getting Claude to write the paper stub was like
"explaining your problem to an MIT teddy bear" — the classic
rubber-duck debugging effect. But there's a specific structure that
made it work:

1. **Enumerate actors as numbered steps** with named transitions (not
   hand-waves). Gaps between sub-steps are where bugs hide.
2. **State an explicit hypothesis**, even if wrong. It bounds the
   search space to the right neighborhood.
3. **Name the invariant** you believe is violated. Even the wrong
   invariant directs attention productively.
4. **Sleep on it.** Session separation provided fresh eyes — the
   writer was anchored to the TLA+ plan; the reader saw the
   catch-block issue directly.

This was codified into both the project's and the global CLAUDE.md as
a repeatable debugging process: write a structured problem description
before reaching for heavier tools. The act of explaining is the
diagnostic tool; the document is a side effect.

## Results

- Fix: 2 lines moved in `spawn_entry`, 3 lines moved in `try_map.h`
- macOS: 636/636 tests, dist 628/628
- Linux ARM64 with `MALLOC_PERTURB_=42`: 10/10 targeted runs, 5/5
  full suite — previously ~1/10 SIGSEGV
- TLA+ specs: `CatchBlockMigration.tla` (no violation) +
  `_Bug.tla` (violation in 5 steps)
- Full write-up with diagnostic appendix in
  `docs/papers/09-spawn-hamt-race.md`
