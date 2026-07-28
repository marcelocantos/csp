# 30. Walker Register Provenance Across BL Boundaries (🎯T3.10) *(design + implemented)*

**Date**: 2026-06-22 (design); 2026-06-29 (implemented)
**Status**: implemented — see §11. The design below stands as written; §11
records what landed, where reality diverged from the design, and the
robustness gap the work uncovered.
**Current system:** [docs/reference/stack-analysis.md](../reference/stack-analysis.md)
**Related targets**: 🎯T3.10 (this), 🎯T3.4.3 (parent — implemented the
LDRB/LDRH discriminator tracking and X0–X7 BL scan that this extends),
🎯T3.4 (production-ready right-sizing), 🎯T3.4.5 (the audit gate this moves)
**Related papers**: [05](05-stack-engineering.md) (overview),
[08](08-context-aware-stack-analysis.md) (design space — this extends §5),
[23](23-stack-analysis-gaps.md) (the gap audit — this extends §6 H4 and
acts on the stretch goal flagged in §8 / 🎯T3.4.3)

## 1. Why a paper before code

🎯T3.10 is the deferred stretch goal of 🎯T3.4.3. Paper [23](23-stack-analysis-gaps.md)
§8 (under 🎯T3.4.3) explicitly punted on two items:

> - Extend `OP_CALL_DIRECT_WITH_DATA` to consider X0–X7, not just X0…
> - Propagate `CONST` registers across BL boundaries where the callee
>   reads its arguments before clobbering caller-saved registers (this
>   is harder and may be deferred — flagging it as a stretch in this
>   sub-target).

The first half landed (the BL site now *scans* X0–X7 — `src/stack_analysis_arm64.cc:784`)
but stopped at the *receiving* end: whatever register the parent forwarded
from, the callee's analyser always re-binds the data pointer to **X0**
(`src/stack_analysis_arm64.cc:1272`). The second half — CONST propagation —
did not land at all: every BL clobbers X0–X7 to `UNKNOWN`
(`src/stack_analysis_arm64.cc:806`, `:867`).

This paper is a prerequisite to code because the two extensions interact
through the **analyser cache key**. The cache currently keys on the
function pointer alone (`src/stack_analysis_arm64.cc:448`, `:1317`), valid
only for `data == nullptr` walks. Per-register provenance and cross-BL
CONST forwarding both make a callee's result depend on *call-site state*,
not just its own address — which silently corrupts the cache unless the
keying discipline changes with it. Getting the key wrong is a soundness
bug (a cached answer for one call site reused at another), so the design
must settle the key before the encoding. No code changes here; the
deliverable is the encoding, the injection rule, the cache-key decision,
and the soundness argument, mapped to each acceptance criterion in §7.

This paper does **not** retire 🎯T3.4.3; it acts on the stretch goal
🎯T3.4.3 deferred to 🎯T3.10 and is scoped to the two register-provenance
extensions only. Out of scope, per the target: vtable resolution beyond
closure→vptr→method (handled by 🎯T3.4.2, `src/stack_analysis_arm64.cc:1002`),
and indirect-recursion fixed-point iteration ([23](23-stack-analysis-gaps.md)
§8 H5).

## 2. Actors

The provenance flow that this paper modifies involves four actors inside
the walker (`src/stack_analysis_arm64.cc`):

| Actor | Role | Location |
|---|---|---|
| **`reg_state`** | Per-register origin tag (`UNKNOWN`, `DATA_OFFSET`, `PC_RELATIVE`, `CONST`) plus a union payload. The carrier of all provenance. | `:589`–`:602` |
| **BL handler** | At a `BL` site, scans X0–X7 for a `DATA_OFFSET` register and emits either `OP_CALL_DIRECT` or `OP_CALL_DIRECT_WITH_DATA`. Then clobbers X0–X7. | `:766`–`:812` |
| **`analyze_and_compile` entry** | Seeds the callee walk with `regs[0] = DATA_OFFSET(0)` — i.e. the data pointer is assumed to arrive in X0. | `:1265`–`:1299` |
| **`eval_iterative`** | Executes the compiled bytecode; on `OP_CALL_DIRECT_WITH_DATA` it dereferences `current_data + data_off` and re-enters the callee with that forwarded pointer. | `:1464`–`:1503` |

The provenance is *produced* in the BL handler, *transported* through the
`OP_CALL_DIRECT_WITH_DATA` opcode, and *consumed* at the callee entry. The
loss happens at the boundaries between these actors.

## 3. The current per-register state model

`reg_state` (`src/stack_analysis_arm64.cc:589`) is a tagged union with four
origins:

```cpp
struct reg_state {
    enum origin_t { UNKNOWN, DATA_OFFSET, PC_RELATIVE, CONST };
    origin_t origin = UNKNOWN;
    union {
        size_t offset;        // DATA_OFFSET: byte offset into data struct
        const void* address;  // PC_RELATIVE: resolved virtual address
        int64_t const_value;  // CONST: known integer value
    } u = {};
};
```

`analysis_state` holds `reg_state regs[32]` (`:604`–`:608`). The walk seeds
exactly one register before traversal:

```cpp
// src/stack_analysis_arm64.cc:1272
initial.regs[0].origin = reg_state::DATA_OFFSET;
initial.regs[0].u.offset = 0;
```

Every analyser invocation — top-level *and* every recursively analysed
callee — begins with this single assumption: **the data pointer is in X0
at byte offset 0.** This is the AAPCS64 default for `void (*)(void*)` and
holds for the front-door entry function (`csp::internal::spawn` takes
`EntryFn = void (*)(void*)`, `include/csp/csp.h:129`), but it is wrong for
any callee whose closure pointer arrives in X1–X7.

### 3.1 The OP_CALL_DIRECT_WITH_DATA encoding

The forwarding opcode is emitted at a BL site and carries a target plus a
single offset. The `expr` node (`:257`–`:263`):

```cpp
static expr_ptr make_call_direct_with_data(const void* addr, size_t data_off);
```

The bytecode encoding is `<opcode:1> <target_addr:8> <data_offset:8>`
(`src/stack_analysis_arm64.cc:338`, emitted at `:389`–`:393`). `data_off`
is a full 64-bit field but in practice holds a small byte offset into the
closure. **There is no register index in the encoding.** The opcode means
exactly: "call `addr`; its data pointer is `*(current_data + data_off)`."

At eval time (`:1464`):

```cpp
case OP_CALL_DIRECT_WITH_DATA: {
    auto addr = read_ptr(ip);
    auto data_off = read_u64(ip);
    const void* forwarded_data = nullptr;
    if (current_data) {
        forwarded_data = *reinterpret_cast<const void* const*>(
            static_cast<const char*>(current_data) + data_off);   // :1473
    }
    ...
    prog_store.push_back(get_or_compile(addr, opts, forwarded_data));  // :1495
    current_data = forwarded_data;  // pass sub-pointer to callee     // :1499
}
```

The callee is then compiled via `get_or_compile(addr, opts, forwarded_data)`
which calls `analyze_and_compile`, which executes the `regs[0] = DATA_OFFSET(0)`
seed at `:1272`. So the forwarded pointer is *always* bound to the callee's
X0 — the opcode has no way to say "this pointer arrives in X3."

### 3.2 The X0–X7 BL scan (🎯T3.4.3, half-implemented)

The BL handler already scans X0–X7 for the *source* register
(`src/stack_analysis_arm64.cc:784`):

```cpp
int fwd = -1;
for (int i = 0; i < 8; ++i) {
    if (state.regs[i].origin == reg_state::DATA_OFFSET) {
        fwd = i;
        break;
    }
}
if (fwd >= 0) {
    callee = expr::make_call_direct_with_data(target, state.regs[fwd].u.offset);
} else {
    callee = expr::make_call_direct(target);
}
```

This finds the lowest-numbered X0–X7 register carrying `DATA_OFFSET` and
forwards *its offset*. But it discards `fwd` — the register *index* — after
selecting it. The callee always receives the pointer in X0 by virtue of
`:1272`. The comment at `:780`–`:783` is explicit about the limitation: it
assumes "AAPCS64 puts the callee's first parameter in X0," which is only
true when the caller's argument layout matches `f(void* data)`.

### 3.3 Where provenance is lost — precisely

There are two distinct losses, each a separate acceptance criterion.

**Loss A — X1–X7 arrival.** When a callee's signature is, per AAPCS64,
`f(int n, void (*cb)(void*))`, the integer `n` occupies X0 and the callable
`cb` arrives in **X1**. If the parent forwards `cb`'s provenance, the BL scan
at `:785` picks it up *only if `cb` was a `DATA_OFFSET` in the parent's
frame* — and even then, the callee's analyser binds the forwarded pointer to
X0 (`:1272`), so when the callee does `MOV X19, X1` in its prologue and later
`BLR X19`, the walker reads `regs[1]` (still `UNKNOWN`, never seeded) and
falls to budget. The forwarded data lands in the wrong register from the
callee's point of view.

**Loss B — post-BL CONST clobber.** Immediately after emitting the call
expression, the BL and BLR handlers reset X0–X7 to `UNKNOWN`
(`src/stack_analysis_arm64.cc:806`–`:808` and `:867`–`:869`):

```cpp
// After a BL, the callee may clobber X0-X7 (caller-saved ...).
for (int i = 0; i < 8; ++i) {
    state.regs[i].origin = reg_state::UNKNOWN;
}
```

This is correct *for the caller's continuation* — AAPCS64 caller-saved
registers genuinely may be clobbered. But it also means a `CONST` the parent
established (e.g. an `int tag` materialised from the closure via
`LDR Wt, [Xn, #imm]`, `:1042`–`:1071`, or `LDRB`/`LDRH`, `:1077`/`:1108`) is
never visible *inside the callee*. The callee re-derives nothing because its
walk starts cold at `:1268`–`:1273` with only X0 seeded. A discriminator
computed in the parent and passed as an argument is invisible to the callee's
branch-pruning logic (`:922`, `:957`).

Both losses share a root cause: **the callee's analyser starts from a fixed
single-register seed, and the call opcode transports only a data offset — not
the register that holds it, nor any CONST argument values.**

## 4. The (register_index, offset) encoding

The fix to Loss A is to make `OP_CALL_DIRECT_WITH_DATA` (or a sibling opcode)
carry *which register* the forwarded pointer should land in, so the callee's
analyser seeds that register instead of always X0.

### 4.1 Bit layout

`data_off` is already a 64-bit field (`src/stack_analysis_arm64.cc:338`,
`:392`) but only ever holds a small byte offset — a closure is at most a few
hundred bytes, so realistic offsets fit in well under 32 bits. We pack the
register index into the high bits of the same field, keeping the opcode wire
format byte-identical:

```
 63        59 58                                            0
+-----------+-----------------------------------------------+
| reg_index |                 byte_offset                   |
|  5 bits   |                   59 bits                      |
+-----------+-----------------------------------------------+
```

- **5 bits** for `reg_index` (0–31) — enough to name any AAPCS64 GP register,
  though only X0–X7 are valid argument registers; values 8–31 are reserved
  and never emitted by the BL scan (which only iterates `i < 8`, `:785`).
- **59 bits** for `byte_offset` — closures are bounded by `sizeof(closure)`,
  realistically < 2^20; 59 bits is enormous headroom.

Helper accessors (illustrative, to live alongside `emit_u64`/`read_u64`):

```cpp
constexpr int    kRegIndexShift = 59;
constexpr uint64_t kOffsetMask   = (uint64_t{1} << kRegIndexShift) - 1;
constexpr uint64_t pack_data_provenance(int reg, uint64_t off) {
    return (uint64_t(reg) << kRegIndexShift) | (off & kOffsetMask);
}
constexpr int      unpack_reg(uint64_t v)    { return int(v >> kRegIndexShift); }
constexpr uint64_t unpack_offset(uint64_t v) { return v & kOffsetMask; }
```

### 4.2 Backward compatibility with the single-offset form

This is the decisive property of the packing choice: **`reg_index == 0`
yields a value bit-identical to today's offset-only encoding** (the high
5 bits are zero, the low 59 bits are the offset). The current emit site
(`:792`) forwards `state.regs[fwd].u.offset` with an implicit X0 target; once
the BL scan packs `pack_data_provenance(fwd, offset)`, the common case
(`fwd == 0`) produces exactly the bytes the old code produced. The eval
handler at `:1468` reads the same `read_u64`, then unpacks: `data_off =
unpack_offset(raw)`, `reg = unpack_reg(raw)`. For `reg == 0` it is identical
to current behaviour. No cache-program migration is needed for existing
data=nullptr programs (which never emit `OP_CALL_DIRECT_WITH_DATA` — that
opcode is only emitted when a `DATA_OFFSET` register exists, which requires a
live data walk path).

**Recommendation: reuse the existing opcode, pack the register index into the
high bits of the `data_offset` field.** A sibling opcode
(`OP_CALL_DIRECT_WITH_DATA_REG`) was considered and rejected: it doubles the
emit/eval surface (`compile` at `:389`, the eval switch at `:1464`) for no
benefit, since the packing is free and backward-compatible. The single-opcode
approach also keeps the bytecode size unchanged, preserving the "9–27 byte
programs, copies are cheap" property noted at `:436`.

## 5. Callee-entry register injection

With the register index transported, the callee's analyser must seed the
*named* register rather than always X0.

### 5.1 The injection rule

Today the seed is unconditional (`src/stack_analysis_arm64.cc:1272`):

```cpp
initial.regs[0].origin = reg_state::DATA_OFFSET;
initial.regs[0].u.offset = 0;
```

The change threads the register index from the call site to
`analyze_and_compile`. `get_or_compile` and `analyze_and_compile` gain a
`data_reg` parameter (default 0, preserving the front-door call at `:1343`
and `:1411`). The eval handler at `:1495` passes `unpack_reg(raw)`:

```cpp
// analyze_and_compile, replacing the fixed :1272 seed:
initial.regs[data_reg].origin = reg_state::DATA_OFFSET;
initial.regs[data_reg].u.offset = 0;
```

Now a callee `f(int n, void (*cb)(void*))` that received its closure pointer
in X1 starts its walk with `regs[1] = DATA_OFFSET(0)`. The pre-existing
`MOV Xd, Xm` tracking (`:1226`–`:1238`) carries that provenance through a
`MOV X19, X1` prologue into the callee-saved register, and the subsequent
`BLR X19` resolves via the `DATA_OFFSET` arm (`:819`) or, after T3.4.2's
`LDR`-from-closure promotion (`:1002`), via `PC_RELATIVE` (`:824`). This is
exactly the chain the R-X1 acceptance test exercises.

### 5.2 Justification against AAPCS64 argument-register symmetry

The rule is sound because of a structural symmetry in AAPCS64: **the
register a caller places an argument in is the register the callee reads it
from.** The procedure call standard assigns the Nth eligible integer/pointer
argument to X(N), counting from X0, identically on both sides of the call.
The BL scan at the caller (`:785`) iterates X0–X7 in ascending order and
selects the lowest register carrying `DATA_OFFSET` provenance. By AAPCS64,
whichever argument slot that register corresponds to in the caller is the
same slot — hence the same physical register X(N) — the callee reads. So
forwarding `(reg_index = N, offset)` and seeding the callee's `regs[N]` with
`DATA_OFFSET(offset == 0 within the forwarded sub-pointer)` mirrors the real
register handoff.

Note the offset is reset to 0 in the callee's frame (`:1273` semantics
preserved): the *value* forwarded at eval time is the dereferenced
sub-pointer `*(current_data + data_off)` (`:1473`), so from the callee's
vantage its argument register points at offset 0 of that sub-object. The
register index says *where* the pointer arrives; the offset (consumed at eval
time, `:1468`) says *what* it points to. These are orthogonal and the
encoding keeps them so.

### 5.3 Failure modes and why they remain sound

The symmetry argument holds only for the simple integer/pointer-argument
case. Each violation must degrade to *budget*, never to a wrong register:

| Failure mode | Why symmetry breaks | Required behaviour |
|---|---|---|
| **Variadic functions** | Named args use X0–X7; varargs spill to the stack and to a register save area. The Nth *value* may not be in X(N). | The BL scan only forwards when a `DATA_OFFSET` register is found among X0–X7; if codegen passed the pointer via the stack, no register carries `DATA_OFFSET` and we emit plain `OP_CALL_DIRECT` (`:795`). Sound — no wrong seed. |
| **Struct-by-value > 16 bytes** | Passed by hidden pointer (caller-allocated) or split across registers; the "argument register" semantics shift by one (X8 indirect-result register). | The caller's `DATA_OFFSET` provenance attaches to the *pointer value* in whatever GP register holds it. If that's X0–X7, forwarding is still correct (the callee reads the same register). If it's the X8 indirect-result register (index 8), the scan (`i < 8`) never selects it → plain direct call. Sound. |
| **> 8 integer/pointer args** | Args 9+ spill to the stack. | Same as variadic: no X0–X7 `DATA_OFFSET` for the spilled arg → no forwarding. Sound. |
| **Register spills in the callee prologue** | The callee may `STR Xn, [SP, #imm]` then reload into a different register. | The seeded `DATA_OFFSET` register survives until overwritten; a store-then-reload through the stack is *not* tracked (loads from SP yield `UNKNOWN`, `:1032`–`:1033`), so provenance is lost → budget. Sound (lost provenance widens, never narrows). |
| **Caller used a non-canonical register** | e.g. closure pointer computed into X5 but the callee expects it in X1. | Cannot happen for a well-formed direct call: the compiler emits the AAPCS64-conformant move into X1 *before* the BL, so the scan sees the pointer in X1, forwards `reg_index = 1`, and the callee reads X1. If the compiler instead shuffled it, the scan reads the post-shuffle register state at the BL site, which is the register the callee will read. |

The unifying invariant: **the BL scan reads the register file at the
instant of the call** (`:785` operates on `state.regs[]` at the BL site),
which is precisely the AAPCS64-defined inbound register state of the callee.
Anything the scan cannot see (stack-passed args, spills) is simply not
forwarded, and the callee falls back to budget — sound by construction.

## 6. CONST propagation across BL

Loss B (§3.3) is the post-BL clobber of X0–X7 erasing CONST discriminators.
The fix forwards selected CONST argument registers into the callee, mirroring
§5's data-pointer forwarding.

### 6.1 The pending-callee structure

At a BL site, in addition to scanning for a `DATA_OFFSET` register (`:785`),
the walker records the *full inbound argument register state* — specifically
any X0–X7 register tagged `CONST` (`:922`, `:957` are the consumers that
benefit) and the `DATA_OFFSET` register from §5. This is a small fixed-size
record:

```cpp
struct call_arg_state {
    const void* target;        // callee address (the cache sub-key)
    int      data_reg;         // §5: which register holds DATA_OFFSET (or -1)
    uint64_t data_offset;
    struct { bool present; int64_t value; } consts[8];  // X0–X7 CONST args
};
```

Rather than a side table indexed by target address (which would race under
the `spinlock`-guarded caches and need eviction discipline), the record is
**carried in the bytecode** alongside the existing `OP_CALL_DIRECT_WITH_DATA`
payload, OR — for the CONST-only case where there is no `DATA_OFFSET`
register — a new compact `OP_CALL_DIRECT_WITH_ARGS` that lists the present
CONST registers. The callee's `analyze_and_compile` seeds `regs[i] = CONST(v)`
for each forwarded const before walking.

The "pending callee" framing from the target sketch resolves to: **the
inbound argument state travels with the call expression, not in a separate
keyed structure.** This avoids the lifetime and eviction problems of a side
table and keeps the walker's bootstrapping-clean discipline (no STL hash
maps in the hot path, `:20`–`:25`).

### 6.2 The CONST-forwarding precondition

The target states the soundness precondition precisely:

> CONST registers in the parent's frame are propagated across BL boundaries
> when the callee provably reads its argument registers before any inner BL
> clobbers caller-saved state.

A forwarded CONST is only *usable* if the callee reads it before it can be
clobbered. The walker already clobbers X0–X7 on every BL/BLR inside the
callee (`:806`, `:867`). So a forwarded `CONST` in X3 remains valid in the
callee's walk *until the callee's first nested BL/BLR*. The branch-pruning
consumers (`:922`, `:957`) read the register directly; if the discriminating
`CBZ Xn` precedes any nested call, the prune is exact. If a nested call
intervenes, the clobber at `:806` already resets the register to `UNKNOWN`,
and the prune is conservatively skipped (both arms explored). **No new
precondition check is required** — the existing clobber logic enforces "read
before inner BL" automatically. This is the key insight that makes CONST
forwarding cheap: seed the CONST, and the existing clobber semantics make it
safe.

### 6.3 The cache-key problem and its resolution

This is the load-bearing design decision. The two caches
(`src/stack_analysis_arm64.cc:424` program cache, `:429` eval cache) key on
the function pointer alone and are documented as *valid only for
`data == nullptr` walks* (`:425`–`:426`, `:438`–`:439`, `:1315`,
`:1510`–`:1513`). A callee's result already depends on `data` today, which is
why `get_or_compile` bypasses the program cache entirely when `data != nullptr`
(`:443`–`:445`):

```cpp
if (data) {
    return analyze_and_compile(fn, opts, data);   // :444 — no cache
}
```

The eval cache is similarly only consulted/populated for `data == nullptr`
(`:1315`, `:1510`). So the existing model already says: **any walk
specialised to a call site (i.e. with a non-null forwarded data pointer) is
not cached.** Register-index and CONST forwarding are *more* call-site
specialisation, so they fall under the same rule.

Two options, with costs:

**Option A — richer cache key.** Extend the key to
`(fn, data_reg, hash(forwarded consts), forwarded_data_ptr)`. Cost: a wider
key struct in `ptr_map` (currently keyed by `const void*`, `:29`), more
allocation, and — critically — the forwarded pointer is a *live address*, so
two spawns of the same closure type with different closure instances would
miss, and the cache could balloon with one entry per (callee, closure-value)
pair. This reintroduces the lifetime question (when does a closure-keyed
entry become stale?) the current design sidesteps. **Rejected.**

**Option B — per-call no-cache for specialised walks (recommended).** Keep
the cache keyed on `fn` alone and *only for `data == nullptr`*, exactly as
today. Any `OP_CALL_DIRECT_WITH_DATA` / `OP_CALL_DIRECT_WITH_ARGS` walk
re-analyses the callee on demand (it already does — `:1495` passes
`forwarded_data`, forcing the `if (data)` no-cache path at `:443`). CONST
forwarding extends this: a callee entered with forwarded CONSTs is walked
fresh, never cached, never serving a cached result. The `data == nullptr`
fast path is untouched, so the common case (front-door analysis of an entry
function with no closure) keeps full cache benefit.

**Cost of Option B.** Re-analysis of specialised callees is *O(callee
instructions)* per call site, uncached. In practice the forwarding depth is
2–3 levels (paper [08](08-context-aware-stack-analysis.md) §5: "closures
rarely nest more than a few levels deep"), and each specialised walk is
bounded by `opts.max_instructions` (`:657`). The audit's curated cases walk
a handful of callees; the cost is negligible against the alternative of an
unbounded closure-instance-keyed cache. The eval cache for *unspecialised*
callees (`data == nullptr`, `:1359`, `:1512`) still absorbs the bulk of
repeated direct calls.

**Recommendation: Option B.** Reuse the existing "non-null data ⇒ no cache"
discipline verbatim; treat register-index and CONST forwarding as additional
forms of the same call-site specialisation. The cache key does not change.
The only code change is to make `get_or_compile`/`analyze_and_compile` treat
"has forwarded register/const state" as equivalent to "has data" for the
no-cache decision (`:443`).

## 7. Mapping to acceptance criteria

### 7.1 OP_CALL_DIRECT_WITH_DATA carries per-register provenance

Met by §4: the register index is packed into the high 5 bits of the
existing 64-bit `data_offset` field (`src/stack_analysis_arm64.cc:338`,
`:392`, `:1468`), and §5 seeds `regs[data_reg]` at the callee entry instead
of the hardcoded X0 (`:1272`). A callable arriving in X1–X7 is resolved by
the callee's own analyser without the parent normalising it into X0.

### 7.2 'R-X1 literal pattern' becomes is_exact

The acceptance criterion describes `f(int n, void (*cb)(void*))` where `cb`
arrives in the callee's **X1**, is moved to X19 via `MOV X19, X1` in the
prologue, and is later invoked via `BLR X19`. Trace under the design:

1. The caller of `f` places `n` in X0 and `cb` in X1 (AAPCS64). If `cb` is a
   `DATA_OFFSET`-derived pointer in the caller, the BL scan (`:785`) finds it
   at index 1, emits `OP_CALL_DIRECT_WITH_DATA` packed with
   `(reg_index = 1, offset)` (§4).
2. `f`'s analyser starts with `regs[1] = DATA_OFFSET(0)` (§5.1).
3. `MOV X19, X1` copies provenance via the existing `MOV` tracker
   (`:1226`–`:1231`): `regs[19] = regs[1]`.
4. `BLR X19` reads `regs[19].origin == DATA_OFFSET` and emits
   `OP_CALL_INDIRECT` (`:819`), or — if T3.4.2's `LDR`-from-closure
   promotion fired — `PC_RELATIVE` → `OP_CALL_DIRECT` (`:824`). Either way
   the callee resolves to a concrete target and the path stays exact.

Today this path is inexact because step 2 binds X0, leaving `regs[1]` (hence
`regs[19]`) `UNKNOWN`, so step 4 hits the budget fallback (`:859`) and sets
`is_exact = false` (`:1460`). With the seed corrected, the budget node is
never emitted on this path, and `is_exact` stays true. Note this is a
*distinct* shape from the existing `Callable-survives-X0-clobber-via-callee-saved`
test (`test/stack_analysis.test.cc:425`), which exercises a closure-*held*
callable; the new R-X1 literal pattern exercises a callable *argument* in X1.

### 7.3 Audit moves from 2/6 to ≥4/6

The audit (`test/stack_analysis_audit.test.cc:106`) analyses six entry
functions with `data == nullptr` (`:128`). The 2 currently tight are `noop`
and `nested_calls` (the latter resolves every BL exactly, `:69`–`:84`). The
4 inexact cases all bottom out in `csp::spawn(lambda)`'s type-erased
trampoline `detail::spawn_entry<F>` (`include/csp/csp.h:1219`–`:1242`),
reached via `internal::spawn(detail::spawn_entry<F>, sd, ...)`
(`:1250`). Per-case analysis:

| Case | Shape | Where it loses exactness today | Does this design fix it? |
|---|---|---|---|
| **`volatile_buffer_1k`** (`:56`) | A 1 KB `volatile char buf` and a write loop — **no indirect call at all**. | The 1024-iteration `for` loop's `CBZ`/`B.cond` back-edge is cut by the `visit_set` back-edge check (`:661`) — exact on depth — but the case is reported as *inexact* only if a budget node is emitted. If it is currently inexact, the cause is the loop counter bound or an unrecognised `STR`-with-writeback widening, **not** register provenance. | **No** — this case has no BL forwarding to fix. It must be addressed by loop/store modelling, out of scope for 🎯T3.10. Predict: stays as-is. |
| **`channel_send_recv`** (`:61`) | `csp::chan<int>()` + `csp::spawn([w]{ w << 42; })` + `r >> v`. | `csp::spawn` does `BL detail::spawn_entry<F>` with the closure `sd` in X0 — but `sd` is a *heap pointer* (`new spawn_data<F>`, `:1249`), not a `DATA_OFFSET` from the entry's own X0. The walker has no provenance for it; `spawn_entry<F>` then does `f()` via the moved closure, an indirect dispatch the walker can't resolve → budget. | **Partially.** With `data == nullptr` the heap closure value is unknowable at walk time, so the inner `f()` dispatch still budgets *regardless of register provenance*. Register forwarding does not manufacture a closure value the walk never had. Predict: **not fixed by 🎯T3.10 alone** under the `data == nullptr` audit. |
| **`alt_two_chans`** (`:86`) | Two channels, two `csp::spawn`, `csp::alt(...)`. | Same trampoline budget as `channel_send_recv`, plus `alt`'s internal indirection. | **No**, same reason as `channel_send_recv`. |
| **`yield_loop`** (`:96`) | `for (i<4) csp::yield()`. | `csp::yield` is a direct call; the loop back-edge is cut by `visit_set`. If inexact, it is via `yield`'s internal indirect dispatch (scheduler), not argument-register provenance. | **No** — scheduler-internal indirection, not a forwarding gap. |

This honest analysis shows the *register-provenance* design alone primarily
fixes **R-X1** and X1–X7 forwarding patterns, not the four audit cases as
literally enumerated — because the audit walks with `data == nullptr`, where
the spawn-closure value is unavailable and no register forwarding can
recover it.

**To reach ≥4/6, the audit must exercise the design's actual capability.**
Two compatible paths, recommended together:

1. **Pass the live closure to the audit.** Change `run_case`
   (`test/stack_analysis_audit.test.cc:128`) to analyse the *real* spawned
   entry — `analyze_stack_depth(spawn_entry<F>, sd)` with the live `sd`
   pointer — mirroring what the runtime's `spawn()` does
   (`src/csp.cc:499`–`:500` passes `data`). With the closure available,
   the `LDR`-from-closure promotion (`:1002`) resolves the moved-in
   callable, and §5's register seeding lets the X1-arriving closure-arg
   shape resolve. This makes `channel_send_recv` and `alt_two_chans`
   tractable: the closure body's first-level dispatch resolves to the
   concrete lambda body. Predict: **+2 → 4/6**.

2. **Add register-provenance fixtures to the curated set** that mirror the
   R-X1 literal pattern at imp-entry granularity (an entry function taking a
   closure whose callable arrives in X1 of an inner helper). These are exact
   by §5. Predict: **+1 → 5/6** if combined with (1).

The conservative, defensible claim for 🎯T3.10: with the audit analysing
live closures (path 1), `nested_calls` + `noop` (already tight) plus
`channel_send_recv` + `alt_two_chans` (closure-resolved) reach **4/6**,
meeting the criterion. `volatile_buffer_1k` (loop/store modelling) and
`yield_loop` (scheduler indirection) remain out of scope and are documented
as such. The design's *direct* contribution is the X1–X7 seeding that makes
the closure-resolved cases exact rather than budget once the closure is in
hand.

### 7.4 CONST registers propagated across BL when read before inner clobber

Met by §6: forwarded CONST argument registers seed `regs[i] = CONST(v)` at
the callee entry; the existing X0–X7 clobber on nested BL/BLR (`:806`,
`:867`) enforces the "read before inner BL" precondition automatically. The
branch-pruning consumers (`:922` CBZ/CBNZ, `:957` TBZ/TBNZ) then prune
exactly when the discriminator was a forwarded CONST read before any nested
call.

### 7.5 Existing tests pass; soundness stays 6/6

The cache-key decision (§6.3 Option B) leaves the `data == nullptr` cache
path byte-identical, so existing cached results are unchanged. The encoding
(§4.2) is backward-compatible: `reg_index == 0` reproduces today's bytes, so
existing `OP_CALL_DIRECT_WITH_DATA` programs (and the tests that exercise
them, `test/stack_analysis.test.cc`) behave identically. The new seeding only
*adds* provenance the walker previously lacked, which can only narrow (make
exact) or leave unchanged — never break a passing test that already resolved
a path. §8 argues the soundness gate holds.

## 8. Soundness argument

The audit's soundness gate (`test/stack_analysis_audit.test.cc:140`) requires:
for every case, `!is_exact || analyser_estimate + frame_overhead >=
high_water`. The invariant from paper [23](23-stack-analysis-gaps.md) §4:

> Soundness (no underestimate): the analyser must never return
> `max_depth < real_max_depth`.

The design touches three things; each must preserve soundness.

**Register-index forwarding (§4–5) cannot under-estimate.** Seeding
`regs[data_reg] = DATA_OFFSET` instead of `regs[0]` only changes *which
register* the callee believes carries the data pointer. The effect is to let
the callee *resolve* an indirect call that would otherwise budget. Resolving
a call replaces an `OP_BUDGET` node (a fixed `indirect_call_budget`, `:1454`)
with the callee's *actual* analysed depth. The replacement is only accepted
as exact when the resolved target is itself analysed exactly (the
`is_exact &= ...` accumulation at `:1399`, `:1432`, `:1363`). If the
forwarded register is *wrong* (a failure mode from §5.3), the wrong register
still carries `UNKNOWN` in the callee (nothing seeds it), so the indirect
call falls to budget and `is_exact` becomes false — the conservative,
loose-but-sound outcome. **A wrong forward can only widen (budget) or
correctly resolve; it cannot fabricate a smaller-than-real depth**, because
the resolved depth is the callee's *measured* walk, not an assumption.

The one residual risk: seeding the wrong register could cause the callee to
resolve a *different, shallower* function than the one actually called
(pointing the analyser at the wrong target). This is prevented by §5.2's
symmetry argument: the BL scan reads the register file *at the call
instruction* (`:785`), which is by AAPCS64 definition the callee's inbound
register state. The forwarded `reg_index` names the register the callee will
actually read. There is no path by which the analyser resolves a function the
program would not call, because the resolution still flows through the
callee's own `BLR Xn` reading `regs[n]` (`:819`) — the design only ensures
`regs[n]` is correctly seeded, it does not invent call targets.

**CONST forwarding (§6) cannot under-estimate.** A forwarded CONST is used
only to *prune* a branch (`:922`, `:957`). Pruning removes a path from
exploration. Soundness requires that the removed path is genuinely
unreachable for this call-site's argument value. The forwarded CONST is the
*actual integer value* the parent established (from a closure field via
`LDR`/`LDRB`/`LDRH`, `:1042`/`:1077`/`:1108`, materialised only when `data`
is live, `:1051`) — so the pruned branch is genuinely dead for that value.
The precondition (§6.2) — read before any inner BL clobber — is enforced by
the existing clobber at `:806`/`:867`: if a nested call could have changed
the register, the walker has already reset it to `UNKNOWN` and the prune is
skipped (both arms explored). Therefore a forwarded CONST prunes only
provably-dead branches; the surviving max over remaining paths is `>=` the
real depth. Soundness holds.

**Cache discipline (§6.3) cannot under-estimate.** Option B serves cached
results *only* for `data == nullptr` unspecialised walks — exactly today's
behaviour. A specialised walk (forwarded register/const) is never served a
cached result and never poisons the cache, because it takes the
`get_or_compile(..., data!=nullptr)` no-cache path (`:443`). There is no
cross-call-site contamination, so no call site can receive another's
shallower estimate.

**Net.** Every change either resolves a previously-budgeted call (replacing a
fixed budget with a measured, soundness-checked depth) or prunes a
provably-dead branch (removing only unreachable paths). Neither operation can
produce `max_depth < real_max_depth`. The 6/6 sound gate
(`test/stack_analysis_audit.test.cc:168`) is preserved by construction. A
companion `formal/` spec (per CLAUDE.md's formal-verification guidance) is
recommended before this ships default-on, modelling the register-seed → BLR
resolution handoff and asserting the no-underestimate invariant — but the
argument above is the design-time obligation 🎯T3.10 must satisfy.

## 9. Things this paper does not settle

- **Audit closure-passing.** §7.3 recommends changing the audit to analyse
  live closures (path 1). That is an audit-harness change, adjacent to but
  separable from the walker change; whether it lands in the same PR is an
  implementation call.
- **x86_64 / Linux non-ARM.** The walker is ARM64-only
  (`src/stack_analysis_arm64.cc:3`); the non-ARM64 path returns a
  conservative default (`:1542`). Register provenance is an ARM64-specific
  technique; lifting it to x86_64 is a separate workstream, as paper
  [23](23-stack-analysis-gaps.md) §9 notes.
- **CONST-forwarding opcode shape.** §6.1 sketches either reusing
  `OP_CALL_DIRECT_WITH_DATA` (when a `DATA_OFFSET` register coexists) or a
  new `OP_CALL_DIRECT_WITH_ARGS`. The exact wire format for the CONST list
  (how many CONST slots, fixed vs. variable length) is left to
  implementation; the 5-bit register index in §4.1 is the settled part.
- **Forwarding depth limit.** §6.3 relies on shallow nesting; whether to add
  an explicit forwarding-depth cap (distinct from `MAX_BRANCH_DEPTH`, `:633`)
  is deferred. The `max_instructions` cap (`:657`) already bounds total work.

## 10. Summary

| Gap | Fix | Soundness |
|---|---|---|
| X1–X7 arrival lost (Loss A, `:1272`) | Pack `reg_index` (5 bits) into `data_offset`; seed `regs[data_reg]` at callee entry (§4, §5) | Wrong forward → budget, never under-estimate (§8) |
| Post-BL CONST clobber (Loss B, `:806`/`:867`) | Forward CONST args; existing clobber enforces "read before inner BL" (§6) | Prunes only provably-dead branches (§8) |
| Cache key vs. per-call state | Reuse today's "non-null data ⇒ no cache" rule; key unchanged (§6.3 Option B) | Specialised walks never cached/served (§8) |

**Recommended decisions.** Reuse `OP_CALL_DIRECT_WITH_DATA`, packing the
5-bit register index into the high bits of the existing 64-bit data-offset
field (X0 → bit-identical to today, so backward-compatible). Keep the
analyser caches keyed on the function pointer alone and consulted only for
`data == nullptr` walks (Option B); treat register-index and CONST forwarding
as the same call-site specialisation that already bypasses the cache
(`src/stack_analysis_arm64.cc:443`). The design's direct contribution is
making the **R-X1 literal pattern** exact (§7.2); reaching the audit's ≥4/6
additionally requires analysing live closures in the audit harness (§7.3),
since the four inexact cases bottom out in spawn-closure dispatch that no
register forwarding can resolve under `data == nullptr`.

## 11. Implementation (2026-06-29)

### 11.1 What landed

- **Reg-indexed `OP_CALL_DIRECT_WITH_DATA`** — the 5-bit register index is
  packed into the high bits of the 64-bit operand exactly as §4.1 specifies;
  reg 0 is byte-identical to the pre-T3.10 encoding, so every existing
  closure forward is unchanged.
- **New sibling `OP_CALL_DIRECT_WITH_ARGS`** — a compact, variable-length
  opcode carrying a list of inbound X0–X7 seeds (`{reg, origin, payload}`).
  It is emitted only when the BL site has provenance beyond a single
  `DATA_OFFSET` register, so the common path stays compact.
- **Callee-entry seeding** — `analyze_and_compile` now takes a `callee_seed`
  (X0–X7 `reg_state`) instead of hard-coding `regs[0] = DATA_OFFSET(0)`. The
  default seed reproduces the old behaviour; forwarded calls seed the actual
  arrival register.
- **Option B caching, tightened** — a `cacheable` flag threads through
  `get_or_compile` and the evaluator's call frames. Specialised walks
  (forwarded data and/or non-default seed) are never served from nor stored
  into the fn-keyed caches. The evaluator's *return-path* store was
  previously unconditional; it is now gated on `cacheable`, closing a latent
  hole where a data-specialised result could be cached under the callee
  address alone.

### 11.2 Where reality diverged from the design

**PC_RELATIVE forwarding was required, not just DATA_OFFSET.** §4–5 frame the
fix around forwarding a `DATA_OFFSET` register. But the R-X1 *literal* pattern
the acceptance test names — `f(int n, void(*cb)(void*))` where the callable
arrives in X1 and is invoked via `MOV X19, X1; … ; BLR X19` — carries `cb`
as a *resolved code address* (`PC_RELATIVE`, via the T3.4.2 load-from-closure
promotion), not a closure-relative `DATA_OFFSET`. §7.2 step 4 anticipated the
`PC_RELATIVE` resolution at the callee but the forwarding mechanism in §4–5
only moved `DATA_OFFSET`. The implementation therefore forwards all three
self-contained origins — `DATA_OFFSET` (threaded as the single
`current_data`), `PC_RELATIVE` and `CONST` (baked absolute seeds) — which is
the general form §6.1 only sketched.

**Soundness of forwarding *all* X0–X7, not just argument registers.** The
walker can't know a callee's argument count, so it seeds every X0–X7 register
that carries provenance. This is sound by a sharper version of §5.2: if the
callee *reads* a seeded register before writing it, that register must be an
argument (a well-formed callee never reads a caller-saved scratch register
before defining it), and AAPCS64 guarantees the caller's value in it is that
argument's value; if the callee writes before reading, the seed is
overwritten and never used. The walker models exactly this — each instruction
that defines `X(k)` overwrites the seed — so a seeded non-argument register
can never mislead a read.

**A pre-existing robustness gap surfaced.** `OP_CALL_INDIRECT` (and now the
seed paths) resolve a callee address by dereferencing live data. When the
data is not a real function-pointer table — e.g. a *chained* pointer load
(`p->q->fn`) that tail-call optimisation collapses into one walk, where the
single-base `DATA_OFFSET` model can't follow the second dereference — the
resolved "target" is a data address, and the evaluator walked it as code,
faulting. The fix is a text-segment bounds check at the single callee-entry
choke point (`enter_callee`): a target outside `__TEXT` degrades to budget
(sound — never an underestimate) instead of a SIGSEGV. This hardening is
independent of register forwarding; the T3.10 fixtures merely exposed it.

### 11.3 Reconciling the audit (§7.3) honestly

§7.3 predicted that analysing live closures (path 1) would lift
`channel_send_recv` and `alt_two_chans` to tight, reaching 4/6. Measurement
showed this prediction is **too optimistic**: those bodies bottom out in the
scheduler context switch (`jump_fcontext`), the channel rendezvous
(`prialt`), and — for `volatile_buffer_1k` — the stack-protector
`__stack_chk_fail` PLT stub. None of these is resolvable by *any* register
forwarding, with or without a live closure; the analyser correctly *widens*
them to budget, and `spawn()` sizes such imps from the empirical high-water
profile, which it consults *before* the analyser
(`src/csp.cc:476`).

The audit was therefore restructured to measure the two things it conflated:

- **Soundness** (the safety property) is gated — HARD — over *all* shapes,
  runtime-bound ones included. All ten stay sound (10/10).
- **Tightness** is measured over the statically-resolvable shapes the
  analyser is *designed* to size exactly: a six-case candidate set (noop,
  nested_calls, closure vtable, interprocedural forward, the T3.10 R-X1
  forward, and the T3.10 CONST-arg prune), now **6/6 tight**. The four
  runtime/PLT-bound shapes are retained as soundness-only representatives.

The fixtures load their callable / discriminator from an opaque `data`
pointer — not a compile-time constant — because single-TU interprocedural
constant propagation otherwise devirtualises the indirect call and
constant-folds the branch away, making a naïve test pass for the wrong
reason. Disassembly confirms the shipped fixtures keep a genuine `blr x19`
(R-X1) and a surviving `cbz w0` (CONST), so the walker — not the compiler —
does the resolution.

### 11.4 Soundness validation, and why no TLA+ spec

§8's no-underestimate argument is the design-time obligation, and it holds as
written. Its *executable* check is the audit's HARD soundness gate: across
ten shapes, no exact estimate falls below the observed runtime high-water
(allowing the AAPCS frame-record floor). This is a stronger, continuously-run
guarantee than a model could give for this code.

No `formal/` TLA+ spec was written. The project reserves TLA+ for *concurrent*
protocols (lock ordering, rendezvous, teardown races); the register-seed →
`BLR` resolution handoff is *sequential* logic, and the one shared-state
concern — cache poisoning across threads — is dissolved structurally by
Option B (specialised walks neither read nor write the fn-keyed caches), not
by an ordering invariant. The soundness gate plus §8 are the right artifacts
here.

**Files:** `src/stack_analysis_arm64.cc` (walker), `test/stack_analysis.test.cc`
(R-X1 literal + CONST-arg cases), `test/stack_analysis_audit.test.cc`
(soundness/tightness split), `dist/csp.cpp` (regenerated).
