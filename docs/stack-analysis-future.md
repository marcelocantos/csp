# Stack Depth Analyzer: Future Directions

Ideas that could push the ARM64 walker beyond what is already shipped.

**Current system (authoritative):**  
[`docs/reference/stack-analysis.md`](reference/stack-analysis.md) — as of
v0.27.0 / 🎯T3.10. Read that page for capabilities, wiring, soundness, and
what is *already* implemented (interprocedural data / X0–X7 provenance,
PC-relative + vtables, profile budgets, spawn Small slots, etc.).

This file is **forward-looking only**. Several sections below pre-date
T3.4.2–T3.10; treat overlapping “problems” as historical design notes unless
the open-gap table in the reference page still lists them.

### Remaining high-value gaps (summary)

| Gap | Notes |
|-----|--------|
| Type-erased `csp::spawn(lambda)` trampoline | Main reason real imps stay Default |
| x86_64 walker | Still a 32 KiB inexact stub |
| Dynamic `SUB SP, Xn`, jump tables | Still limited (FP/SIMD stack ops landed in 🎯T52.1 — see §3/§4) |
| Mutual indirect-recursion fixed point | Explicitly deferred |

---

## 1. Multi-level data propagation

> **Status:** largely **landed** via 🎯T3.4.3 / 🎯T3.10 (`OP_CALL_DIRECT_WITH_DATA`
> and per-register seeds). Kept for design history; see the reference page.

### Problem (historical)

When `eval_iterative` entered a direct callee via `CALL_DIRECT`, it set
`current_data = nullptr`. Nested `CALL_INDIRECT` then fell back to budget.

### Design (as proposed; now largely implemented)

Extend the bytecode VM's call frame to carry a **data context** through direct
calls:

```
struct eval_frame {
    ...
    const void* caller_data;    // data pointer from the caller's scope
    uint64_t    data_offset;    // accumulated offset at point of call
};
```

At `BL` sites, the walker already knows which register holds each argument
(ARM64 calling convention: X0–X7). If X0 at the call site has `DATA_OFFSET`
provenance, the walker can emit a new opcode:

```
OP_CALL_DIRECT_WITH_DATA  <target_addr>  <data_offset>
```

The evaluator would then set `current_data = (char*)caller_data + data_offset`
when entering the callee, allowing the callee's `CALL_INDIRECT` opcodes to
resolve against the forwarded pointer.

### Scope

This primarily benefits the common pattern where a top-level entry function
passes a closure pointer to a helper, and the helper calls through a captured
function pointer:

```cpp
// spawn entry: data points to the closure
void entry(void* data) {
    auto* closure = static_cast<Closure*>(data);
    closure->setup();        // direct call — data is still live
    closure->body(data);     // body receives data, calls through it
}
```

### Risks

- Propagating data through arbitrary call chains could cause the evaluator to
  dereference stale or invalid pointers if the analyzed program reinterprets
  the memory. Mitigate by only propagating when the walker can prove X0
  provenance is a direct pass-through (no arithmetic beyond ADD of constants).
- Increases bytecode complexity. A fallback to `OP_CALL_DIRECT` (no data)
  keeps the common path unchanged.

---

## 2. PC-relative address resolution (ADRP+ADD / ADRP+LDR)

### Problem

ARM64 accesses globals and vtables via PC-relative sequences:

```asm
ADRP  X8, _vtable@PAGE
ADD   X8, X8, _vtable@PAGEOFF
LDR   X9, [X8, #offset]      ; load function pointer from vtable
BLR   X9
```

The walker currently marks X8 as `UNKNOWN` after ADRP, losing the chain. This
is one of the most common sources of inexactness in optimized code.

### Design

Add a new register origin: `PC_RELATIVE(resolved_address)`.

```cpp
struct reg_state {
    enum origin_t { UNKNOWN, DATA_OFFSET, PC_RELATIVE };
    origin_t origin = UNKNOWN;
    union {
        size_t offset;             // DATA_OFFSET
        const void* address;       // PC_RELATIVE: resolved base address
    };
};
```

**ADRP decoding:**

```cpp
// ADRP Xd, label — bits [23:5] = immlo, [20:29] = immhi
if (match(inst, 0x9F000000, 0x90000000)) {
    uint32_t rd = inst & 0x1F;
    int64_t immhi = sign_extend((inst >> 5) & 0x7FFFF, 19);
    int64_t immlo = (inst >> 29) & 0x3;
    int64_t imm = (immhi << 14) | (immlo << 12);
    auto page = reinterpret_cast<uintptr_t>(state.pc) & ~0xFFFULL;
    if (rd < 31) {
        state.regs[rd].origin = reg_state::PC_RELATIVE;
        state.regs[rd].address = reinterpret_cast<const void*>(page + imm);
    }
    state.pc++;
    continue;
}
```

The subsequent `ADD Xd, Xn, #imm` and `LDR Xt, [Xn, #imm]` handlers would
propagate `PC_RELATIVE` the same way they propagate `DATA_OFFSET`, adjusting
the resolved address by the immediate.

When `BLR Xn` is hit with a `PC_RELATIVE` register, the walker can resolve the
target at walk time (the address is fully determined) and emit `CALL_DIRECT`
instead of `CALL_INDIRECT`. This converts an inexact indirect call into an
exact direct call with zero evaluator changes.

### Scope

This would resolve the majority of currently-inexact BLR instructions in
optimized code. Covers:

- C++ virtual calls (vtable dispatch)
- PLT/GOT stubs (dynamic linking)
- Global function pointer tables
- `std::function` internal dispatch

### Risks

- Requires the analyzer to read memory at the resolved address (the vtable
  or GOT entry). This is safe for code loaded in the current process but
  would need bounds checking if analyzing code in a different address space.
- ADRP resolution depends on knowing the PC at walk time, which is always
  available since the walker tracks `state.pc`.

---

## 3. FP/SIMD stack operations

> **Status: landed** (🎯T52.1, 2026-07-30). The full load/store-pair
> writeback class — GP *and* FP/SIMD, W/X/S/D/Q, pre- and post-indexed —
> is decoded structurally (one handler keyed on the class encoding, scale
> from V/opc), covered by a hand-written asm audit fixture whose exact
> peak is asserted. Anything in the class that is not decodable (STGP,
> unallocated opc) is refused by the closed-world SP-write detector
> (budget + `is_exact=false`) rather than skipped. Kept for design
> history.

### Problem (historical)

Functions that save/restore NEON registers use 128-bit STP/LDP:

```asm
STP  Q0, Q1, [SP, #-64]!    ; save 4 × 128-bit registers
...
LDP  Q0, Q1, [SP], #64      ; restore
```

These modify SP but are not decoded, so the SP delta drifts and all
subsequent depth estimates are wrong (not just conservative — wrong in
either direction).

### Design

Add decoders for the FP/SIMD STP/LDP pre/post-indexed forms. The encoding
differs from the GP forms only in the top bits and the scale factor (16 bytes
per register pair instead of 8):

| Form | Encoding mask | Scale |
|------|--------------|-------|
| STP Dt, Dt2, [Xn, #imm]! (64-bit FP pre) | `0xFFC003E0` / `0x6D800000 \| (Rn<<5)` | ×8 |
| LDP Dt, Dt2, [Xn], #imm (64-bit FP post) | `0xFFC003E0` / `0x6CC00000 \| (Rn<<5)` | ×8 |
| STP Qt, Qt2, [Xn, #imm]! (128-bit FP pre) | `0xFFC003E0` / `0xAD800000 \| (Rn<<5)` | ×16 |
| LDP Qt, Qt2, [Xn], #imm (128-bit FP post) | `0xFFC003E0` / `0xACC00000 \| (Rn<<5)` | ×16 |

Only the SP-modifying forms matter (Rn = 31). The handler adjusts `sp_delta`
by `sign_extend(imm7, 7) * scale`.

### Scope

Low risk, high value. These are common in any function that uses floating
point or SIMD. The decoder is mechanical — same structure as the existing
GP STP/LDP handlers with different bit patterns and scale factors.

---

## 4. Pre/post-indexed STR/LDR (single-register)

> **Status: landed** (🎯T52.1, 2026-07-30). The load/store register imm9
> writeback class (GP and FP/SIMD, all widths) is decoded; asm audit
> fixtures assert exact depths for the X/W/D/Q forms. Kept for design
> history.

### Problem (historical)

The compiler sometimes uses single-register pre/post-indexed forms instead
of paired STP/LDP:

```asm
STR  X30, [SP, #-16]!     ; push LR
...
LDR  X30, [SP], #16       ; pop LR
```

These modify SP but are not decoded.

### Design

Add decoders for:

| Form | Encoding | Scale |
|------|----------|-------|
| STR Xt, [Xn, #simm]! (pre-index, 64-bit) | `0xFFE00C00` / `0xF8000C00` | 1 (simm9) |
| LDR Xt, [Xn], #simm (post-index, 64-bit) | `0xFFE00C00` / `0xF8400400` | 1 (simm9) |
| STR Wt, [Xn, #simm]! (pre-index, 32-bit) | `0xFFE00C00` / `0xB8000C00` | 1 (simm9) |
| LDR Wt, [Xn], #simm (post-index, 32-bit) | `0xFFE00C00` / `0xB8400400` | 1 (simm9) |

The immediate is a 9-bit signed value (no scaling). Only SP-targeting forms
(Rn = 31) adjust `sp_delta`.

### Scope

Straightforward. Covers edge cases where the compiler chooses single-register
push/pop, typically for leaf functions or when only LR needs saving.

---

## 5. Switch/jump table support

> **Status: deferred** (2026-07-30 appraisal). Pending the 🎯T52.2 corpus
> metric — until the Small-rate baseline shows how often jump tables are
> the blocking inexactness source on real workloads, the bounded-table-scan
> risk (reads past an under-determined table bound) isn't worth taking.

### Problem

Optimized switch statements compile to an indirect branch through a jump
table:

```asm
ADRP  X9, _table@PAGE
ADD   X9, X9, _table@PAGEOFF
LDR   W10, [X9, X8, LSL #2]    ; load relative offset
ADD   X9, X9, W10, SXTW        ; compute target
BR    X9                         ; indirect jump
```

The `BR X9` terminates the path with an inexact budget because the walker
can't enumerate the table entries.

### Design

**Phase 1 — Bounded table scan.** If the walker can identify the pattern
(ADRP+ADD establishing a base, then LDR with register index, then BR), it
can:

1. Resolve the table base via `PC_RELATIVE` tracking (§2).
2. Read table entries up to a configurable limit (e.g., 256 entries).
3. For each entry, compute the target address and push it onto the worklist
   as an alternative branch target.
4. Take the MAX across all targets.

**Phase 2 — Pattern matching.** Define a small set of recognized idioms
(Clang and GCC each have characteristic jump table sequences). Rather than
full symbolic execution, match the instruction window preceding `BR Xn`
against known templates.

### Risks

- Table bounds are not always statically determinable. A wrong bound could
  cause reads beyond the table into unrelated data. Mitigate with a hard
  cap and by verifying that all computed targets fall within the function's
  address range (or within a reasonable distance).
- Multiple switch table idioms exist across compiler versions. Start with
  the Clang ARM64 pattern (which CSP is compiled with) and add others
  incrementally.

---

## 6. Dynamic SP adjustment tracking

### Problem

`SUB SP, SP, Xn` (register-based stack allocation) immediately bails to
budget. This occurs with VLAs, `alloca`, and compiler-generated dynamic
alignment.

### Design

**Constant propagation.** If the register holds a known constant (from a
`MOV Xn, #imm` or a chain of arithmetic on constants), the walker can
resolve it exactly. Add a `CONST(value)` register origin:

```cpp
struct reg_state {
    enum origin_t { UNKNOWN, DATA_OFFSET, PC_RELATIVE, CONST };
    ...
    int64_t const_value;   // CONST: tracked value
};
```

Track `MOVZ`, `MOVK`, `MOV Xd, #imm` (ORR with zero register and shifted
immediate), and arithmetic on known-constant registers. When `SUB SP, SP, Xn`
is hit and Xn is `CONST`, apply the adjustment exactly.

**Dynamic alignment.** The common pattern:

```asm
MOV   X9, SP
AND   X9, X9, #-alignment
MOV   SP, X9
```

Can be recognized as reducing SP by at most `alignment - 1` bytes. Emit a
conservative-but-bounded adjustment rather than the full budget.

### Scope

Constant propagation covers the most common case (fixed-size VLAs with
compile-time-known bounds). True dynamic allocation (`alloca(n)` with
runtime `n`) remains inherently unknowable without range analysis.

---

## 7. Callee argument forwarding

### Problem

The data propagation (§1) only forwards the root data pointer. But many
real call patterns pass *derived* pointers:

```cpp
void process(State* s) {
    s->handler(s->ctx);    // BLR with s->handler; data for callee is s->ctx
}
```

The callee's own indirect calls need `s->ctx`, not `s`.

### Design

Extend `CALL_INDIRECT` to carry a **data-forwarding descriptor** — a second
offset indicating which field of the current data should become the callee's
data:

```
OP_CALL_INDIRECT_WITH_DATA  <fn_offset>  <data_offset>
```

The walker emits this when it can see that X0 at the BLR site has
`DATA_OFFSET` provenance (meaning the callee receives a data-derived
pointer). The evaluator sets:

```cpp
current_data = *(void**)((char*)current_data + data_offset);
```

before entering the callee.

### Risks

Each level of forwarding is a pointer dereference into the analyzed
program's memory. Stale pointers (from programs that mutate the data
between calls) would cause incorrect resolution. Limit forwarding depth
to a configurable maximum (e.g., 3 levels).

---

## 8. x86_64 support

### Problem

The analyzer is ARM64-only. On x86_64, `analyze_stack_depth` returns
`{32KB, false}` — a conservative default that wastes memory for small
closures and underestimates for deep call chains.

### Design

x86_64 presents different challenges:

- **Variable-length instructions**: Need a length decoder or disassembler
  library. Options: a minimal decoder for the subset of instructions that
  affect RSP (PUSH, POP, SUB RSP, ADD RSP, CALL, RET, Jcc), or integrate
  a lightweight disassembler like Zydis (header-only option available).
- **Stack frame patterns**: `PUSH RBP` / `SUB RSP, imm` / `ADD RSP, imm` /
  `POP RBP` / `RET`. Simpler than ARM64 in some ways (fewer addressing
  mode variants) but more complex in others (REX prefixes, SIB bytes).
- **Calling convention**: System V AMD64 ABI. RDI = first arg (equivalent
  to ARM64's X0 for data pointer tracking).

### Approach

Start with a minimal RSP-tracking walker that handles:

1. `PUSH reg` / `POP reg` (SP ∓ 8)
2. `SUB RSP, imm32` / `ADD RSP, imm32`
3. `CALL rel32` → `CALL_DIRECT`
4. `CALL [reg]` / `CALL [rip+disp]` → `CALL_INDIRECT` or resolved
5. `RET` → terminate path
6. `Jcc rel8/rel32` → fork both paths

This covers the vast majority of compiler-generated code. The x86_64
walker would share the expression tree, bytecode compiler, and evaluator
with ARM64 — only the instruction decoder differs.

### Scope

Moderate effort. The variable-length encoding is the main complexity.
Consider generating the decoder from an instruction table rather than
hand-coding each pattern.

---

## 9. Improved budget heuristics

> **Status: cut** (2026-07-30 appraisal). Budget *magnitude* has no
> consumer: any budget contribution clears `is_exact`, and an inexact
> result always selects the Default slot — tiering the constant changes
> nothing downstream. The "recursive cycle → 0" row was also unsound as
> stated (a cycle's frames do consume stack; only the *analysis* is
> cut off there). Kept for design history.

### Problem (historical)

When the analyzer can't resolve a path, it assigns a flat
`indirect_call_budget` (default 2048 bytes). This is either too generous
(wasting stack for trivial indirect calls) or too conservative (not enough
for deep virtual call chains).

### Design

**Tiered budgets.** Replace the single budget with per-situation defaults:

| Situation | Budget | Rationale |
|-----------|--------|-----------|
| Unresolvable BLR with no register info | 2048 | Current default |
| BLR through GOT/PLT stub | 512 | PLT stubs have ~0 stack usage; the real callee is the concern |
| Dynamic SP adjustment (SUB SP, Xn) | 4096 | Could be a large VLA |
| Over-budget path (max_instructions exceeded) | 2048 | Analysis timed out |
| Recursive cycle detected | 0 | Cycle doesn't add stack — recursion is bounded by the RT |

**Statistical refinement.** After the initial wave of analysis (during
runtime initialization), compute the 95th percentile of exact depths across all
analyzed functions. Use this as a data-driven budget for subsequent inexact
analyses, rather than an arbitrary constant.

---

## 10. Incremental analysis and warm-up

> **Status: cut** (2026-07-30 appraisal). A background symbol-table sweep
> conflicts with the runtime's zero-syscall hot-path ethos (standing
> analysis work and cache churn for functions that may never spawn), and
> is superseded by the async-worker direction (🎯T52.4: Default-on-miss +
> a dedicated analysis worker), which warms exactly the entries that are
> actually spawned. Kept for design history.

### Problem (historical)

`analyze_stack_depth` is called from system-thread spawns (which can
afford the cost) but `analyze_stack_depth_cached` is used for
imp spawns (where latency matters). If a function hasn't been
analyzed yet, the imp gets the 32 KB conservative default.

### Design

**Background analysis.** During runtime initialization, spawn a background thread
that walks the program's symbol table (via `dl_iterate_phdr` on Linux or
the Mach-O load commands on macOS) and pre-analyzes all functions in the
text segment. Results populate `g_eval_cache` so that subsequent
`analyze_stack_depth_cached` calls hit warm cache.

**Priority queue.** Rather than analyzing all functions, maintain a queue
ordered by call frequency (instrumented via a lightweight counter on
`CALL_DIRECT` evaluations). Analyze hot functions first.

### Risks

- Symbol table walking may include functions that are never called,
  wasting analysis time. The priority queue mitigates this.
- Background analysis must be thread-safe. The existing spinlock-based
  caches already support concurrent access.

---

## 11. Hierarchical eval caching

### Problem

The eval cache (`g_eval_cache`) is keyed by function address alone. When
`data` is non-null, the evaluator bypasses the cache entirely because the
same function can produce different depths depending on the indirect call
targets that `data` resolves. This means every data-dependent analysis
re-evaluates the bytecode from scratch.

Benchmarking shows the impact:

| Path | Latency |
|------|---------|
| Warm eval (cache hit, no data) | ~5 ns |
| Warm eval (with data, cache miss) | ~142 ns |
| Cache-only (`analyze_stack_depth_cached`) | ~3 ns |

The 28× slowdown on the data path matters for high-frequency spawn patterns
where the same closure type is spawned repeatedly with different captures
but identical function-pointer layouts.

### Observation

The bytecode program for a given function is already cached per function
pointer (`g_cache`). The program itself is deterministic — it encodes the
function's control flow and stack adjustments. The only variable input is
`data`, and it only affects evaluation at `OP_CALL_INDIRECT` sites, where
the evaluator dereferences `data + offset` to get a concrete callee address.

The individual callee results *are* cached (the evaluator checks
`g_eval_cache` for each resolved target). So on a warm second call, the
bytecode runs, hits CALL_INDIRECT, resolves the target, finds the callee
in the cache, and continues — the "re-evaluation" is really just
replaying the bytecode's MAX/ADD arithmetic over cached callee results.

The missing piece is caching the *root function's combined result* for a
given set of resolved targets, so that even the bytecode replay can be
skipped.

### Design

Keep the top-level cache keyed on function pointer alone. Extend it with
a **per-function sub-cache** for parameter-dependent results:

```cpp
struct fn_cache_entry {
    // Result for data=nullptr (the common case, already cached today).
    stack_analysis no_data_result;
    bool has_no_data_result = false;

    // Sub-cache for data-dependent results, keyed on resolved indirect
    // targets.  Each entry maps a target-set fingerprint to a result.
    ptr_map<stack_analysis> by_targets;  // key = hash of resolved targets
};

// Top-level cache: fn → fn_cache_entry.
ptr_map<fn_cache_entry> g_eval_cache;
```

The evaluation flow becomes:

1. **Lookup by fn.** Check `g_eval_cache[fn]`.
2. **No-data fast path.** If `data == nullptr` and `has_no_data_result`,
   return immediately (~5 ns, unchanged from today).
3. **Resolve indirect targets.** The bytecode program is small (typically
   9–27 bytes). Pre-scan it to extract all `OP_CALL_INDIRECT` offsets,
   dereference each against `data` to get the concrete callee addresses,
   and compute a fingerprint (hash of the sorted target set).
4. **Sub-cache lookup.** Check `fn_entry.by_targets[fingerprint]`. On hit,
   return immediately — no bytecode evaluation needed.
5. **Evaluate on miss.** Run the bytecode as today. Cache the result in
   `fn_entry.by_targets[fingerprint]`.

```cpp
// Pre-scan: extract CALL_INDIRECT offsets and resolve targets.
struct target_info {
    uint64_t offset;
    const void* resolved;
};
small_vector<target_info, 4> targets;
for (const uint8_t* scan = prog.data(); scan < end; ) {
    switch (*scan++) {
    case OP_CALL_INDIRECT: {
        auto off = read_u64(scan);
        auto target = *(const void**)((const char*)data + off);
        targets.push_back({off, target});
        break;
    }
    case OP_PUSH: case OP_BUDGET: case OP_CALL_DIRECT: scan += 8; break;
    case OP_MAX: case OP_ADD: break;
    }
}

// Fingerprint: hash of resolved targets.
size_t fingerprint = hash_combine(targets...);
```

### Alternative: per-function hash table

Instead of encoding the fingerprint as a key in a shared map, each
function's cache entry can own a small dedicated hash table. This has
better locality for functions with many distinct target combinations
(the sub-table stays in the same cache line neighbourhood) and avoids
polluting the global cache's load factor with per-function variants.

The choice between a global map with composite keys vs. per-function
sub-tables is an implementation detail. The per-function approach is
slightly cleaner conceptually — the top-level lookup finds the function,
then the function's own table handles the parametric dimension — and
avoids needing a two-field hash key in the global map.

### Interaction with callee caching

The sub-cache is complementary to the existing per-callee caching. When
a CALL_INDIRECT target has already been evaluated (for any caller), the
callee result is in `g_eval_cache` and the bytecode evaluator reuses it.
The sub-cache avoids even running the bytecode replay — it short-circuits
the entire evaluation for a known (fn, targets) pair.

For programs that spawn many imps with the same closure type but
different captured values, the first spawn evaluates fully, and all
subsequent spawns with the same function-pointer layout hit the sub-cache
in ~5 ns.

### Risks

- **Sub-cache growth.** A function called with many distinct target sets
  could accumulate many sub-cache entries. Mitigate with a cap per
  function (e.g., 64 entries) with LRU or FIFO eviction.
- **Stale entries.** If the program mutates function pointers in a data
  struct between spawns, cached results become invalid. This is already a
  theoretical concern with the existing callee cache and is not a practical
  issue for normal programs (closures are typically immutable after
  construction).

---

## Priority and sequencing

| § | Innovation | Effort | Value | Dependencies |
|---|-----------|--------|-------|-------------|
| 3 | FP/SIMD stack ops | Small | High | None |
| 4 | Pre/post-indexed STR/LDR | Small | Medium | None |
| 2 | ADRP+ADD resolution | Medium | High | None |
| 6 | Dynamic SP (const prop) | Medium | Medium | None |
| 1 | Multi-level data propagation | Medium | Medium | None |
| 5 | Switch/jump tables | Medium | Medium | §2 |
| 7 | Callee argument forwarding | Medium | Medium | §1 |
| 9 | Budget heuristics | Small | Medium | None |
| 11 | Data-aware eval cache keying | Small–Medium | High | None |
| 10 | Incremental warm-up | Medium | Medium | None |
| 8 | x86_64 support | Large | High | None (parallel track) |

Sections 3 and 4 **landed** in 🎯T52.1 (2026-07-30); sections 9 and 10 are
**cut** and section 5 is **deferred** pending the 🎯T52.2 corpus metric —
see the per-section status notes. Section 2 (ADRP) landed earlier
(🎯T3.4.2). Section 8 (x86_64) is independent and can proceed in parallel.
