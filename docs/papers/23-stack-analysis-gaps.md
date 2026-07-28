# 23. Stack Analysis Gap Audit (🎯T3.4 scoping)

**Date**: 2026-05-17
**Status**: historical scoping paper — 🎯T3.4 / T3.4.1–T3.4.5 and 🎯T3.10
are **achieved**. §3’s “analyser unwired” snapshot is obsolete.
**Current system:** [docs/reference/stack-analysis.md](../reference/stack-analysis.md)
**Related targets**: 🎯T3.4 (retired), 🎯T3, 🎯T3.3
**Related papers**: [05](05-stack-engineering.md) (overview),
[08](08-context-aware-stack-analysis.md) (design space),
[20-arena-stack-scaling.md](20-arena-stack-scaling.md) (consumer),
[30](30-walker-register-provenance.md) (T3.10 stretch that closed more gaps)

## 1. Why a paper before code

🎯T3.4's acceptance criteria are research-shaped — "resolves nested
function pointers, vtables, parameter-driven paths", "interprocedural
data flow and profile-guided calibration", "tight without overflows on
real workloads". Each bullet is plausibly a quarter of work and they
interact. Paper [08](08-context-aware-stack-analysis.md) already maps
the design space; this paper closes the loop by:

1. auditing what is actually implemented in
   `src/stack_analysis_arm64.cc` against that design,
2. tracing how (and whether) the analyser's output reaches the runtime
   sizing decision,
3. proposing a decomposition of 🎯T3.4 into sub-targets that can be
   ordered and shipped independently.

No code changes here. The deliverable is the sub-target breakdown at
the end and the updates to `bullseye.yaml` that pair with it.

## 2. Actors

The runtime's stack lifecycle for a single imp involves five actors:

| Actor | Role |
|---|---|
| **User callable `F`** | The function passed to `spawn()`. Compiled to a closure object with a known address and a known entry point. |
| **`csp::detail::spawn()`** (`src/csp.cc`) | Reserves a stack region, places the `Imp` header at the top, calls `make_fcontext`, switches in. |
| **`StackPool`** (`src/stack_pool.cc`, `include/csp/internal/stack_pool.h`) | Owns stack regions. Hands out either an arena slot (124 KB usable + 4 KB soft guard) or a 1 MB mmap'd VM region (per-imp guard page). |
| **Instruction walker** (`src/stack_analysis_arm64.cc`) | The unit-of-study. Walks ARM64 from a function entry, builds a compact bytecode program, and an iterative evaluator computes `max_depth`. |
| **`check_stack_overflow()`** (`include/csp/internal/csp_internal.h`) | Soft-guard tripwire fired at every CSP suspend checkpoint. Aborts the process on touch. |

The hypothesis paper [08](08-context-aware-stack-analysis.md) describes
a richer set of *context* sources the walker could consume — closures,
vtables, globals, register provenance, profile history. The current
walker implements a strict subset of that design.

## 3. Sequence

The intended sequence is:

1. User calls `spawn(F, data)`.
2. Runtime invokes `analyze_stack_depth(F, data)` to compute a budget.
3. `StackPool::allocate(budget)` returns a region sized to the budget
   (rounded up to slot size, plus headroom).
4. Imp runs. On every suspend, `check_stack_overflow` verifies SP is
   still above the soft guard.
5. On exit, the region returns to the pool; `madvise(MADV_FREE)`
   reclaims the pages that were touched.

The actual sequence today is:

1. User calls `spawn(F, data)`.
2. Runtime **does not** consult `analyze_stack_depth`. Sizing is a
   compile-time constant (124 KB arena slot / 1 MB non-arena).
3. `StackPool::allocate()` returns one slot regardless of `F`.
4. Imp runs. Soft-guard works (arena mode).
5. Exit-time reclamation works as designed.

That is, the analyser **is implemented but unwired**: it has a test
suite, an evaluator, and an opcode set, but no production caller. The
gap is bigger than the acceptance criteria suggest — even with a
perfect analyser, no imp gets right-sized today.

## 4. Invariant

> For every imp `I` with entry function `F` and data `d`,
> `allocated_stack(I) ≥ worst_case_used_stack(F, d)`.

Today this holds *trivially* because the allocated stack is constant
and large (124 KB / 1 MB) relative to the depth of any imp in the
existing test suite or examples. Tightening the allocation — the whole
point of 🎯T3.4 in the context of 🎯T3.3 (100K+ imps) — narrows the
margin and shifts the invariant from "obviously safe" to "safe iff the
analyser is sound".

Two related invariants matter:

- **Soundness** (no underestimate): the analyser must never return
  `max_depth < real_max_depth`. Returning a high estimate (loose) is
  acceptable; returning low is a memory-corruption bug.
- **Tightness** (no gratuitous overestimate): the analyser must
  resolve enough indirection that the returned value is close to the
  real maximum, otherwise we cannot meaningfully shrink slots below
  the current 124 KB.

These are in tension. Every gap in the analyser either widens
estimates (preserves soundness, costs memory) or has to fall back to
the fixed slot size (preserves soundness, costs the whole feature).

## 5. What the walker currently does

`src/stack_analysis_arm64.cc` decodes ARM64 by pattern-matching
instruction encodings:

- **Stack ops**: `SUB SP, SP, #imm`, `ADD SP, SP, #imm`, pre-indexed
  `STP [SP, #imm]!`, post-indexed `LDP [SP], #imm`. Tracked exactly.
  Register-based `SUB SP, SP, Xn` falls back to budget — alloca-style
  dynamic frames are not modelled.
- **Direct calls**: `BL imm26` emits `OP_CALL_DIRECT`. The evaluator
  follows the callee at eval time (with cycle detection via
  `small_ptr_set`).
- **Indirect calls (BLR Xn)**: only resolved when `Xn` has
  `DATA_OFFSET` provenance — i.e. the call target was loaded from the
  closure (e.g. `LDR Xn, [X0, #imm]`). Otherwise budget (2 KB default,
  marks the result inexact).
- **Indirect branches (BR Xn)**: same provenance check. PC-relative BR
  (jump-table style) is deliberately not followed — the comment notes
  the risk of walking into GOT/PLT stubs and faulting.
- **Conditional branches**: `B.cond`, `CBZ/CBNZ`, `TBZ/TBNZ`. Both
  arms are pushed to a worklist (depth cap 64). With `data` non-null,
  `CBZ/CBNZ/TBZ/TBNZ` on `CONST` registers prune to the taken arm.
- **Register tracking**: four origins — `UNKNOWN`, `DATA_OFFSET`,
  `PC_RELATIVE` (ADRP+ADD), `CONST` (MOVZ/MOVK, 32-bit LDR from
  closure when `data` is present). The walker is conservative on
  unknown destinations and clobbers `X0–X7` after every call (AAPCS64
  caller-saved).
- **Interprocedural data forwarding**: when X0 at a BL site has
  `DATA_OFFSET` provenance, the walker emits
  `OP_CALL_DIRECT_WITH_DATA <target> <offset>` and the evaluator
  dereferences `data + offset` to materialise the callee's data
  pointer. This is the partial implementation of paper 08 §5.
- **Caching**: per-function bytecode in `g_cache` (keyed by function
  pointer, valid only for `data == nullptr` walks); per-function
  evaluation result in `g_eval_cache` (same key/condition).

The analyser is bootstrapping-clean: open-addressed `ptr_map`,
intrusive `expr_ptr`, no STL containers in the hot path, no `BLR`s in
its own compiled code. This is what makes self-analysis exact, per
paper [05](05-stack-engineering.md) §5.1.

## 6. Hypotheses about where precision is lost

Numbered to align with §7 reproducers and §8 sub-targets.

**H1.** *PC-relative dispatch never resolves to a concrete target.*
`ADRP+ADD` produces a `PC_RELATIVE` register. The walker propagates it
through `LDR` (refining the address) and `ADD` (offset arithmetic),
but the **BLR/BR handlers refuse to dereference it**. The comment
explains why (GOT/PLT stubs can be unmapped or point into system
libraries). The consequence: every vtable call, every PLT-dispatched
external call, every `static const fn_ptr table[] = {…}` lookup
collapses to a 2 KB budget and an inexact result. Paper 08 §11.2
proposed adding a segment-bounds guard before the dereference; that
guard does not exist in code yet.

**H2.** *Vtables are never recognised structurally.* Even if H1 were
fixed, the walker has no notion of "this LDR loads the vtable pointer
from offset 0, and that LDR loads slot N from the vtable". It just
sees two chained `LDR` ops. With H1 fixed and one extra inference rule
("`LDR Xt, [Xn, #imm]` where `Xn = DATA_OFFSET(0)` yields a vtable
pointer at imm; subsequent `LDR Xu, [Xt, #idx]` yields a vmethod
pointer"), the walker can resolve virtual dispatch via the live object
without enumerating the class hierarchy.

**H3.** *Parameter-driven paths require the closure to literally
contain the discriminator.* `data`-aware branch pruning works *only*
when the discriminating value is a 32-bit field loaded from a known
data offset (`LDR Wt, [Xn(DATA_OFFSET), #imm]`). It does not pick up
discriminators passed via:
- 8-bit / 16-bit / 64-bit loads (`LDRB`, `LDRH`, `LDR Xt`),
- discriminators that pass through arithmetic (`AND`, `LSR`, `EOR`),
- discriminators carried in registers across a BL boundary
  (interprocedural value flow),
- discriminators read from a vtable / global / `constexpr` table.

The 32-bit LDR case is the easiest to demo (CBZ on `int tag`); the
other cases are silently widened to MAX.

**H4.** *Interprocedural data flow is one level deep and only on X0.*
The current `OP_CALL_DIRECT_WITH_DATA` propagates the data pointer
across one BL. If the callee further passes a sub-pointer to a
grandchild via X0, this works recursively (the BL site re-reads X0's
provenance). But:
- It only inspects X0, not X1–X7. Functions that pass the callable in
  X1 (e.g. `f(x, callback)`) lose the provenance.
- The data pointer is the only thing forwarded; CONST registers from
  the parent's frame are not propagated across BL.
- When a BL clobbers X0–X7 (caller-saved), all provenance below the
  call is reset. Functions that compute a sub-pointer, call into a
  helper, then re-use the sub-pointer after, will lose tracking.

**H5.** *Recursion is cut off at a budget.* `small_ptr_set` (capacity
64) catches direct recursion at eval time. Indirect recursion through
a function pointer (mutually recursive callbacks) terminates only via
the per-walk `MAX_BRANCH_DEPTH` (64) and `max_instructions` (100K)
caps, both of which mark the result inexact. There is no fixed-point
iteration; recursive functions get the budget.

**H6.** *Profile feedback does not exist.* The runtime knows the
real-time SP at every suspend (`check_stack_overflow` already reads
it) but does not record a high-water mark per entry function. Paper
08 §6 sketched the mechanism; nothing is wired. A profile-guided
budget would be useful even without any other improvement — it
tightens the *budget* number used at the inexact fallback, which is
where most current results bottom out.

**H7.** *The whole pipeline has no production caller.* Paper 05 talks
about the walker "operating at spawn time". `src/csp.cc:454`'s comment
mentions "heap-allocate with stack analyzer right-sizing" — but the
code below it just divides `total_size` by 16, ignoring any analyser
output. The plumbing from analyser → `StackPool::allocate(budget)` →
slot selection is missing. Until this is wired, no improvement to the
walker reduces a single byte of stack reservation. **This is the
ranking-1 gap and is largely independent of H1–H6.**

**H8.** *"Real workloads" is undefined.* The existing
`test/stack_analysis.test.cc` exercises hand-crafted leaves, indirect
callers, interprocedural forwarding, and CBZ-pruning. There is no
benchmark that walks an *actual* CSP entry function — a `serve()`
imp, a `web_crawler` worker, an HTTP/2 handler — and reports the
delta between analyser output and observed high-water mark. Until
that benchmark exists, claims of "tight without overflows on real
workloads" cannot be settled. This is a measurement gap, not a
walker gap, but the acceptance criterion requires it.

## 7. Reproducers

Each reproducer is a small test fixture that should land alongside the
sub-target that fixes the corresponding hypothesis. They are not
written yet; what follows is a sketch.

| ID | Hypothesis | Reproducer shape | Today's result | Target result |
|---|---|---|---|---|
| **R-PC** | H1 | A function that calls a `static void (*const table[])(void*)` entry via ADRP+LDR+BLR. | inexact, 2 KB budget | exact, real depth |
| **R-VT** | H1+H2 | `struct I { virtual void f() = 0; }` with a single concrete subclass; call `obj->f()` through the closure. | inexact | exact, real depth |
| **R-TAG8** | H3 | A `uint8_t tag` field discriminating two paths; closure-aware analysis on each value. | inexact (LDRB not tracked) | exact for each value |
| **R-X1** | H4 | `f(int n, void (*cb)(void*))` — callable passed in X1, called via X19. | inexact | exact when data forwarded |
| **R-MUT** | H5 | Mutually recursive `a(d) → b(d) → a(d)` via two callbacks in the closure. | inexact, depth cap | fixed-point with budget |
| **R-PROF** | H6 | A long-running imp whose actual high-water is ~6 KB; show that the runtime records this and applies it to the next spawn of the same entry function. | no recording | budget refined to 6 KB + margin |
| **R-WIRE** | H7 | A `spawn(F)` call that asks the runtime for the slot it chose and asserts it depends on `F`. | always 124 KB | proportional to `analyze_stack_depth(F)` |
| **R-BENCH** | H8 | A benchmark that for every example in `examples/` reports `(analyser_estimate, runtime_high_water, ratio)`. | n/a | reported and tracked |

## 8. Proposed decomposition

Five sub-targets, ordered by dependency and payoff. 🎯T3.4 itself
remains as a checkpoint over the sub-targets — it retires when they
all retire.

### 🎯T3.4.1 — Stack analyser output drives allocation (H7, R-WIRE)

The unblocker. Wire `analyze_stack_depth` into `spawn()`:

- At spawn time on the *parent's* stack, call
  `analyze_stack_depth_cached(F, data)`.
- If `is_exact && max_depth + headroom <= small_slot`, request a small
  slot from `StackPool`. Otherwise use the existing 124 KB slot.
- Add `kSmallSlot` (e.g. 8 KB or 16 KB) as a second slot class managed
  by the same arena machinery — most imps in `examples/` should land
  here once H1/H3 are fixed.

Cost: **M**. Risk: correctness (a wrong estimate now corrupts
adjacent imps in the arena). Mitigated by:
- only honouring `is_exact` results,
- requiring a generous headroom multiplier (e.g. 2×),
- gating behind a runtime flag `CSP_ANALYSE_STACKS=1` for early
  rollout.

Note: this target *can* land before H1–H4 are fixed; it just won't
shrink many imps until the analyser becomes more precise. That is the
right ordering — the plumbing reveals which gaps matter in practice.

### 🎯T3.4.2 — PC-relative dispatch resolution with segment guard (H1, R-PC, R-VT)

Implement paper 08 §11.2: in BLR/BR handlers, when `Xn` is
`PC_RELATIVE`, check that the resolved address falls within the
binary's own `__TEXT` segment (excluding `__stubs`), then dereference
and emit `OP_CALL_DIRECT`.

- Segment bounds via `_dyld_get_image_header` (macOS) /
  `dl_iterate_phdr` (Linux), computed once at module init.
- Reject targets outside the bounds (fall back to budget).
- Add an explicit secondary rule for the vtable pattern: an LDR
  from `[X0, #0]` followed by an LDR `[Xt, #idx]` resolves the
  vmethod pointer at walk time. This is mechanically the same code
  path as `PC_RELATIVE`, just with the live object's vtable as the
  base.

Cost: **M**. Risk: correctness (dereferencing the wrong address). The
segment guard is the key mitigation — outside the home binary's text
segment, defer to budget.

### 🎯T3.4.3 — Wider value tracking and interprocedural propagation (H3, H4, R-TAG8, R-X1)

Two related extensions:

- Add value tracking for `LDRB Wt, [Xn, #imm]`, `LDRH Wt, [Xn, #imm]`,
  and 64-bit `LDR Xt, [Xn, #imm]` when `Xn` is `DATA_OFFSET` and
  `data` is non-null. Each becomes a `CONST` register. (Already done
  for 32-bit LDR.)
- Extend `OP_CALL_DIRECT_WITH_DATA` to consider X0–X7, not just X0,
  when the callee's first parameter sources from one of those
  registers. (Walker-side change; the opcode already carries enough
  state.)
- Propagate `CONST` registers across BL boundaries where the callee
  reads its arguments before clobbering caller-saved registers (this
  is harder and may be deferred — flagging it as a stretch in this
  sub-target).

Cost: **M**. Risk: correctness (a wrong CONST prunes a real branch
and underestimates depth). The unchanged invariant — only prune when
the load is `is_exact` and the discriminator is known at walk time —
keeps this sound by construction.

### 🎯T3.4.4 — Profile-guided budget calibration (H6, R-PROF)

Per paper 08 §6:

- Sample SP at every suspend (already done by
  `check_stack_overflow`). On suspend, record `max(high_water,
  current_depth)` keyed by entry function.
- On the *next* spawn of the same entry function, use the recorded
  high-water (+ 50% margin) as the *budget* instead of the flat 2 KB
  default. The `is_exact` flag remains false; only the magnitude
  improves.
- Persist nothing (in-memory only). Reset on `set_maxprocs` or
  process restart.

Cost: **S** (basic version) / **M** (per-process persistence not
needed here). Risk: low — this only improves the inexact fallback;
it cannot make a sound analysis unsound.

### 🎯T3.4.5 — Real-workload benchmark and tightness budget (H8, R-BENCH)

A `bench/stack_analysis_bench.cc` that, for each entry function in
`examples/` and a curated set of test cases:

- Runs the imp, samples high-water at termination.
- Calls `analyze_stack_depth(F, &closure)` and records the estimate.
- Reports the ratio (estimate / high-water) and the headroom budget.
- Fails if any analysed-as-exact case underestimates the real
  high-water (soundness CI).

This becomes the empirical acceptance gate for 🎯T3.4. Acceptable
"tight": 95% of imps in `examples/` and the test suite have
`analyser_estimate < 2 × high_water + 4 KB` with `is_exact == true`.

Cost: **M**. Risk: low (it's pure measurement). Provides the
yardstick the original acceptance criteria gestured at.

### Dependencies

```
T3.4.5 (bench)
    ↑ measures
T3.4.1 (wire) ─────────────────────────────────┐
    ↑ unblocks tightness gains from            │
T3.4.2 (PC-rel + vtables)                      │
    ↑ further tightness                        │
T3.4.3 (value tracking, X1–X7 forwarding)      │
    ↑ orthogonal                               │
T3.4.4 (profile feedback)  ────────────────────┘
```

🎯T3.4.1 and 🎯T3.4.5 can ship in either order; the rest can land
incrementally as walker-only changes once 🎯T3.4.1 is in.

Recommended attack order: **🎯T3.4.1 first**. Without it, no other
improvement reduces a single byte of allocated stack; with it, every
subsequent walker improvement immediately becomes observable in the
arena slot mix.

## 9. Things this paper does *not* settle

- **Slot classes**: should there be two slot sizes (small + large) or
  a continuum? Two is the obvious starting point; a continuum
  interacts with arena packing in ways that warrant a separate
  paper.
- **Windows / Linux**: this audit is ARM64-only (the walker is the
  only implementation). Linux x86_64 currently uses the conservative
  default. Lifting any of these techniques to x86_64 is a separate
  workstream (probably a 🎯T3.4.x parallel to 3.4.2 once one
  architecture's design is settled).
- **Sanitiser builds**: the walker is disabled under ASan/TSan
  because instrumented code injects BLR calls that taint every
  result. Profile-guided calibration (🎯T3.4.4) is the only sub-target
  that could plausibly work under sanitisers; the others are gated
  on the static walker.
- **Heuristic vs. proven bound**: paper 08 §7 lists the soundness
  rules for closures, vtables, and globals. Wiring this into a
  runtime that grants smaller stacks based on analyser output makes
  those rules load-bearing. A future paper should formalise them as
  invariants (possibly TLA+ or a small proof obligation list) before
  🎯T3.4.1 ships to default-on.

## 10. Summary

| Hypothesis | Sub-target | Cost | Risk |
|---|---|---|---|
| H7 (no production caller) | 🎯T3.4.1 | M | correctness (mitigated by `is_exact` gate + headroom) |
| H1, H2 (PC-rel + vtables) | 🎯T3.4.2 | M | correctness (mitigated by segment guard) |
| H3, H4 (value tracking, X1–X7) | 🎯T3.4.3 | M | correctness |
| H6 (profile feedback) | 🎯T3.4.4 | S | low |
| H8 (real-workload benchmark) | 🎯T3.4.5 | M | low |
| H5 (indirect recursion) | not split out | — | covered by budget today; revisit if a real workload hits it |

🎯T3.4 retires when all five sub-targets retire and the benchmark in
🎯T3.4.5 shows tight, exact estimates on the example suite.
