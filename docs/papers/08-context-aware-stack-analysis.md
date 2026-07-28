# Context-Aware Stack Depth Analysis

**Current implementation (as shipped):**  
[docs/reference/stack-analysis.md](../reference/stack-analysis.md)

## Abstract

Static stack depth analysis — walking a function's machine instructions to
estimate its maximum stack consumption — is inherently limited by what the
analyzer can see. A compiler-time analysis sees only the code. A profiler
sees actual stack depths but only after the fact. We describe a third
option: **spawn-time analysis**, where the instruction walker runs at the
moment an imp is created and has access to the full runtime state of the
process — live memory, populated vtables, resolved function pointers,
constructed closures. This unique vantage point enables techniques that are
impossible in pure static analysis and unnecessary in pure profiling.
We survey the design space, identify the most impactful opportunities, and
discuss the soundness constraints that make these techniques safe in practice.

## 1. The analysis spectrum

Consider three points at which a program's stack behaviour can be analysed:

**Compile time.** The compiler sees the source code (or IR) and can compute
exact frame sizes for each function. It knows the call graph for direct calls
but cannot resolve indirect calls — function pointers, virtual dispatch, and
callbacks are opaque. The compiler also cannot see across compilation units
without link-time optimisation. Compiler-time analysis produces exact results
for simple programs and conservative estimates for everything else.

**Run time (profiling).** A profiler observes actual stack depths during
execution. It sees everything — every indirect call target, every branch
taken, every recursive depth. But it only reports what happened, not what
could happen. A function that recursed to depth 5 during profiling might
recurse to depth 500 in production. Profiling gives a lower bound, not a
safe upper bound.

**Spawn time.** When an imp is created, the entry function exists as
compiled machine code in the process's address space. The closure (if any)
is a fully constructed object in memory. Vtables are populated. Global
variables have their current values. The instruction walker can read all of
this while traversing the function's control flow graph.

This is the key insight: **spawn-time analysis has access to runtime state
that static analysis lacks, while still operating ahead of execution —
producing upper bounds, not observations.** The function hasn't run yet, so
the analysis can explore all control flow paths. But unlike a compiler, it
can peek into memory to resolve ambiguities that static analysis must
conservatively approximate.

The CSP instruction walker already exploits this partially: it reads the
closure's memory layout to resolve first-level indirect calls through
captured function pointers. But this is the tip of an iceberg. The runtime
context available at spawn time is vastly richer than what the current
implementation consumes.

## 2. Runtime closure inspection

A C++ lambda that captures variables compiles to a struct whose fields are
the captured values. When the lambda captures function pointers, callable
objects, or `std::function` instances, those appear as fields at known
offsets:

```cpp
auto handler = [&process, &cleanup](reader<int> r) {
    for (auto v : r) process(v);
    cleanup();
};
spawn(handler, ch.r);
```

The compiler generates something like:

```
struct __lambda_42 {
    void (*process)(int);   // offset 0
    void (*cleanup)();      // offset 8
    // ...
};
```

At spawn time, the closure exists in memory. The analyzer can read offset 0
to find the address of `process` and offset 8 to find `cleanup`. When the
instruction walker encounters `BLR X9` after loading from one of these
offsets, it resolves the target to a concrete function address and follows it
as a direct call.

The current implementation does this for one level — the root closure passed
as the `data` parameter to the entry function. But closures nest. A captured
`std::function` contains its own type-erased callable, which may itself
capture further function pointers. A captured object pointer leads to an
object whose vtable leads to concrete method implementations.

**Deep closure traversal** follows these chains transitively: read a
function pointer from the closure, follow it, discover that the callee loads
another pointer from the same or a derived data structure, read that pointer,
follow it, and so on. Each level resolves another layer of indirect calls
that static analysis would abandon to a conservative budget.

The practical limit is not conceptual but mechanical: the instruction walker
must track register provenance through each callee to know which loads
correspond to which data offsets. The current register tracking (DATA_OFFSET
provenance for X0-derived loads) provides the foundation; extending it to
propagate through call boundaries converts nested indirect calls from
opaque budgets to exact depths.

## 3. Global state as analysis input

ARM64 code accesses globals and vtables through PC-relative addressing:

```asm
ADRP  X8, _config@PAGE
ADD   X8, X8, _config@PAGEOFF
LDR   W9, [X8]              ; load config.max_depth
```

At compile time, the value of `_config.max_depth` is unknown — it depends on
runtime configuration. At spawn time, the global exists in memory with its
current value. The instruction walker can resolve the ADRP+ADD sequence to
a concrete address, read the value, and use it to inform analysis.

This has several applications:

**Vtable dispatch.** A virtual call loads a function pointer from an
object's vtable. The vtable is a global array of function pointers, fully
populated by the time any imp is spawned. If the instruction walker can
trace the object pointer to a known address (via the closure or a global),
it can read the vtable entry and resolve the virtual call to a concrete
method:

```asm
LDR   X8, [X0]              ; load vtable pointer from object
LDR   X9, [X8, #16]         ; load third virtual method
BLR   X9                     ; dispatch
```

With the object at a known address, the vtable pointer is readable, and the
method address at offset 16 is a concrete function. The `BLR X9` becomes a
direct call.

**Dispatch tables.** Libraries that use function pointer tables for
extensibility (plugin systems, codec registries, command dispatchers)
populate these tables at initialisation. By spawn time, they're stable. An
analyzer that can resolve ADRP sequences to global addresses can read
dispatch table entries and follow them.

**Configuration-dependent code.** Functions that branch on global
configuration flags have different stack depths depending on which branch is
taken. If the flag's value is readable at spawn time, the analyzer could, in
principle, prune the unreachable branch. However, this interacts with
soundness constraints (section 7) — a global that changes between spawns
invalidates the pruning.

## 4. Parameter-driven path pruning

Consider a function that dispatches on a tag:

```cpp
void handle(int tag, Payload* p) {
    switch (tag) {
    case TAG_SMALL: handle_small(p); break;  // 200 bytes stack
    case TAG_LARGE: handle_large(p); break;  // 8000 bytes stack
    case TAG_HUGE:  handle_huge(p);  break;  // 32000 bytes stack
    }
}
```

Static analysis must take the MAX across all cases: 32000 bytes. But if the
spawning code is:

```cpp
spawn([&] { handle(TAG_SMALL, &payload); });
```

the closure captures `TAG_SMALL` as a constant. At spawn time, the analyzer
can see that `tag` is 0 (or whatever TAG_SMALL's value is) and prune the
other branches, yielding 200 bytes instead of 32000. Note that `TAG_SMALL`
need not be a compile-time constant for this to work — even if the
spawning code captures a runtime variable (`int tag = compute_tag();`),
the closure contains the variable's concrete value by the time `spawn()`
is called, and the analyzer can read it from the closure's memory layout.

This requires two capabilities the current analyzer lacks:

1. **Value tracking for non-pointer registers.** The walker currently tracks
   register provenance only for pointer-derived values (DATA_OFFSET). To
   resolve branch conditions, it needs to track concrete integer values
   loaded from the closure or derived from constants.

2. **Condition evaluation.** When a conditional branch (`CBZ`, `TBZ`,
   `B.cond`) depends on a register with a known value, the walker can
   determine which branch is taken and follow only that path.

The combination is powerful for closure-heavy code. Closures commonly
capture configuration flags, enum discriminants, size parameters, and mode
selectors that determine which code paths execute. Each resolved condition
potentially eliminates an entire subtree of worst-case stack depth from the
analysis.

The challenge is knowing when a value is truly constant for the lifetime of
the analysis. A value loaded from the closure at a known offset is safe —
the closure is immutable after construction. A value loaded from a global is
only safe if the global doesn't change between spawn-time analysis and the
imp's actual execution (see section 7).

## 5. Interprocedural data flow

The most significant limitation of single-level data propagation is that
callees lose context. When function `A` calls function `B` and passes the
closure pointer as an argument, `B` receives a pointer it can dereference —
but the current analyzer doesn't tell `B`'s analysis what the pointer
points to.

```cpp
void A(void* data) {
    auto* c = static_cast<Closure*>(data);
    B(c->inner);   // B receives c->inner in X0
}

void B(Inner* p) {
    p->callback(); // BLR — what's p->callback?
}
```

At the top level, the analyzer knows `data` points to the closure. It can
resolve `c->inner` by reading the closure at the appropriate offset. But
when it follows the `BL B` call, the current implementation sets
`current_data = nullptr`, and `B`'s `BLR` falls back to a budget.

**Interprocedural data flow** preserves the data context across call
boundaries. If the walker can see that `X0` at the `BL B` call site is
derived from the root data pointer (e.g., `X0 = *(data + 16)`), it can
forward the resolved address as the callee's data pointer. The callee's
analysis then resolves `p->callback` by reading at the forwarded address.

This is fundamentally a **reaching definitions** problem: which data pointer
reaches each indirect call site? The ARM64 calling convention makes this
tractable — arguments are passed in X0-X7, and the walker already tracks
register provenance. The extension is to propagate provenance across `BL`
boundaries when the argument register has known provenance.

The depth of propagation is bounded by the call chain length and the number
of pointer dereferences the walker can track. In practice, two or three
levels of propagation resolve the vast majority of indirect calls in
closure-heavy code, because closures rarely nest more than a few levels
deep.

## 6. Profile-guided budget calibration

When the analyzer encounters an unresolvable indirect call, it assigns a
flat budget — currently 2048 bytes regardless of context. This is a
guess. It's conservative enough for most cases and insufficient for some.

A better approach uses **empirical calibration**: track the actual stack
depths observed when imps run, and use these observations to inform future
budgets.

The mechanism:

1. **Observation.** At yield points (channel operations, timer waits), the
   runtime already knows the current stack pointer. Recording the
   high-water mark is a single comparison and conditional store.

2. **Per-function statistics.** Aggregate observations by entry function.
   After N spawns of the same function, the runtime has a distribution of
   observed stack depths.

3. **Budget derivation.** Replace the flat budget with a per-function
   estimate derived from the distribution — for example, the 99th
   percentile of observed depths plus a safety margin. This is neither the
   worst-case (which may be pathologically large) nor the average (which
   risks overflow for unusual inputs), but a pragmatic upper bound informed
   by real behaviour.

4. **Feedback loop.** If a new spawn's analysis uses a profile-derived
   budget, and the imp later exceeds the predicted depth (detected at a
   yield-point SP check), the budget is revised upward. The imp doesn't
   crash — it's still within the 1 MB virtual region — but the statistics
   are updated to reflect the new high-water mark.

This is analogous to profile-guided optimisation in compilers, but applied
at runtime to a specific analysis problem. The key advantage is that it's
self-correcting: conservative initial budgets are refined as the program
runs, converging on tight bounds without programmer intervention.

## 7. Soundness and safety

Reading runtime memory during spawn-time analysis raises correctness
questions. The analyzer dereferences pointers into the closure, vtables,
and potentially globals. Under what conditions are these reads safe?

**Closures are safe.** A C++ closure is constructed before `spawn()` is
called and is moved into the imp's ownership. Between construction and the
first instruction of the imp's execution, the closure is not modified by any
other thread. The analyzer runs during this window — after construction,
before execution. Every read of the closure's memory observes the values
that the imp will observe when it runs.

**Vtables are safe.** C++ vtables are populated during static initialisation,
before `main()` runs. They are never modified during program execution (the
standard mandates this for well-formed programs). Reading a vtable entry at
spawn time produces the same function pointer that a virtual call would
dispatch to at any point during execution.

**Globals require care.** A global variable read at spawn time may have a
different value when the imp actually executes the code that depends on it.
If the analyzer uses a global's value to prune a branch, and the global
changes before the imp reaches that branch, the analysis is unsound — it
may have underestimated the stack depth.

The conservative position: **use globals only for address resolution (ADRP
targets), not for value-dependent branch pruning.** This is safe because
the global's address doesn't change (it's a link-time constant), even if
its contents do. Reading a function pointer from a global dispatch table is
safe as long as the dispatch table isn't mutated after initialisation — which
is true for vtables and typical dispatch tables, but not for mutable global
state.

A middle ground is to classify globals by mutability:

- **Immutable after init** (vtables, const globals, dispatch tables
  populated once): safe to read values.
- **Mutable** (configuration flags, counters, state variables): safe to
  read addresses, unsafe to read values for branch pruning.

The analyzer cannot determine mutability from the machine code alone. This
could be addressed by programmer annotation (marking globals as
init-time-constant) or by conservative default (only read through const
pointers).

**Concurrent mutation** is not a concern for closures (owned by the
spawning thread until handoff) but is theoretically possible for globals.
In practice, programs that mutate globals concurrently with spawning are
rare and already have data races. The analyzer's read is no less safe than
the imp's own subsequent read of the same global.

## 8. Towards a unified context model

The techniques described above share a common structure: the analyzer
maintains a **context** — a mapping from memory regions to known values —
and consults this context when it encounters loads, branches, or indirect
calls. The current implementation has a minimal context (the `data` pointer
for root-level closure reads). A unified model would generalise this:

```
Context = {
    closure: address → byte map    (the closure's memory)
    vtables: address → fn_ptr[]    (read-only after init)
    globals: address → value       (classified by mutability)
    registers: X0..X30 → provenance + value
    history: fn_ptr → depth_stats  (profile observations)
}
```

Each technique corresponds to expanding one dimension of this context:

| Technique | Context dimension |
|-----------|-------------------|
| Deep closure inspection (§2) | closure (deeper traversal) |
| Global state resolution (§3) | vtables, globals |
| Path pruning (§4) | registers (value tracking) |
| Interprocedural flow (§5) | registers (cross-call propagation) |
| Profile-guided budgets (§6) | history |

The implementation doesn't need to build this model all at once. Each
dimension is independently valuable and can be added incrementally. The
register provenance system already provides the mechanical framework —
extending the set of tracked origins (`DATA_OFFSET`, `PC_RELATIVE`,
`CONST`) and the propagation rules (cross-call forwarding) is the path
to a richer context.

## 9. Priority and expected impact

Not all techniques are equally valuable. The following ordering reflects
a combination of implementation effort, prevalence of the resolved pattern
in real code, and magnitude of the depth reduction:

1. **ADRP resolution + vtable dispatch** (§3). This is the single highest-
   impact improvement. Optimised C++ code is dominated by virtual calls and
   GOT/PLT dispatch, both of which go through ADRP sequences. Resolving
   these converts the majority of currently-inexact BLR instructions into
   exact direct calls. Medium effort, high payoff.

2. **Interprocedural data flow** (§5). Extends the reach of closure
   inspection to nested calls. Most spawn-time closures involve a thin
   wrapper calling a deeper function with forwarded state. Propagating
   the data pointer through one or two call levels resolves the
   remaining indirect calls that closure inspection alone misses.
   Medium effort, high payoff for closure-heavy code.

3. **Parameter-driven path pruning** (§4). High impact in specific
   scenarios (dispatch functions, tagged unions, mode-dependent code) but
   requires value tracking beyond pointer provenance. Medium effort,
   variable payoff depending on code patterns.

4. **Deep closure traversal** (§2). Straightforward extension of existing
   data propagation. The mechanical work is in tracking pointer chains
   through nested structures. Low-medium effort, moderate payoff.

5. **Profile-guided budgets** (§6). Self-correcting, zero-annotation,
   and useful even without any other improvement — it makes the fallback
   path smarter. Low effort for basic implementation, moderate payoff.

## 10. Related work

Stack depth analysis has a long history in safety-critical systems, where
proving the absence of stack overflow is a certification requirement. The
techniques described in this paper draw on — and depart from — several
threads of prior work.

**Static binary analysis.** [AbsInt
StackAnalyzer](https://www.absint.com/stackanalyzer/index.htm) is the
industrial standard for worst-case stack usage analysis. It works directly on
binary executables, reconstructs control flow, and computes sound upper
bounds on stack consumption per task. It handles indirect calls and recursion
but requires user-supplied annotations when targets cannot be resolved
statically — precisely the gap that spawn-time context fills automatically.
The [WCET community](https://www.cs.fsu.edu/~whalley/papers/tecs07.pdf) uses
similar abstract-interpretation techniques on binary code, sharing the
instruction-walking approach but operating entirely at build time without
access to runtime state.

**Whole-program source-level analysis.** Rust's
[cargo-call-stack](https://github.com/japaric/cargo-call-stack) performs
whole-program static stack analysis using LLVM-IR type information to
approximate indirect call targets. Its author
[acknowledges](https://blog.japaric.io/stack-analysis/) that indirect calls
through function pointers and dynamic dispatch produce incorrect or missing
call graph edges, limiting the tool to embedded programs with minimal
indirection. [Rapita
Systems](https://www.rapitasystems.com/blog/function-pointers-and-their-impact-stack-analysis)
documents the same fundamental problem: when the analysis cannot determine
where an indirect call goes, it must either assume zero stack usage
(unsound) or fall back to a conservative budget (imprecise).

**Context-sensitive pointer analysis.** The compiler literature offers
sophisticated techniques for resolving function pointers at compile time.
[Emami, Ghiya, and
Hendren](https://dl.acm.org/doi/10.1145/773473.178264) describe
context-sensitive interprocedural points-to analysis that handles function
pointers by tracking calling contexts. [Wilson and
Lam](https://www.cs.cmu.edu/afs/cs/academic/class/15745-s09/www/lectures/p1-wilson.pdf)
extend this with partial transfer functions.
[Milanova](http://www.cs.rpi.edu/~milanova/docs/ase_subm.pdf) achieves
precise call graphs for C programs with function pointers. These analyses
are powerful but operate on source or IR at compile time — they reason about
*possible* pointer targets across all executions, whereas spawn-time
analysis reads the *actual* target for a specific invocation.

**Dynamic stack growth.** Go sidesteps the analysis problem entirely:
goroutines [start with 2 KB
stacks](https://blog.cloudflare.com/how-stacks-are-handled-in-go/) and grow
dynamically when the compiler-inserted prologue detects overflow. This
requires compiler cooperation (the stack-check prologue at every function
entry) that is unavailable to a C++ library. The dynamic growth approach
trades analysis for runtime overhead — every function call pays for a stack
bound check, and growth events involve copying the entire stack.

**JIT speculation.** Just-in-time compilers occupy a conceptually similar
position: they have access to runtime type profiles and can speculate about
call targets. [CoSSJIT (OOPSLA
2025)](https://dl.acm.org/doi/10.1145/3763149) combines static dataflow
analysis with speculative optimisation in a JIT, using runtime type
information to guide stack allocation decisions. [Flückiger et
al.](https://dl.acm.org/doi/10.1145/3434327) formalise speculative
deoptimisation, including the reconstruction of stack frames when
speculation fails. These systems use runtime context for code *generation*
rather than code *analysis* — they optimise execution speed, not stack
sizing. Nevertheless, the principle is the same: runtime state resolves
ambiguities that static analysis cannot.

**Where this work differs.** The approach described in this paper combines
elements that have not previously been assembled together: binary-level
instruction walking (as in AbsInt), performed at runtime (as in a JIT),
with direct reads of live process memory (closures, vtables, globals) to
resolve indirect calls that every prior approach either annotates manually,
approximates conservatively, or ignores. The spawn-time window — after the
closure is constructed, before the imp executes — provides a unique vantage
point that is neither compile-time static analysis nor runtime profiling. It
produces sound upper bounds (like static analysis) informed by concrete
state (like profiling), without requiring compiler cooperation (like Go),
user annotations (like AbsInt), or code generation (like a JIT).

## 11. VM implementation sketch

The current bytecode VM has six opcodes (`OP_PUSH`, `OP_MAX`, `OP_ADD`,
`OP_CALL_DIRECT`, `OP_CALL_INDIRECT`, `OP_BUDGET`) and two register
origins (`UNKNOWN`, `DATA_OFFSET`). Supporting the techniques in this
paper requires extensions to both the walker's register tracking and the
VM's opcode set and evaluation state. This section sketches the concrete
changes.

### 11.1 Extended register origins

The walker's `reg_state` currently tracks whether a register was derived
from the data pointer (X0) at a known offset. Three new origins are needed:

```cpp
struct reg_state {
    enum origin_t { UNKNOWN, DATA_OFFSET, PC_RELATIVE, CONST, FORWARDED };
    origin_t origin = UNKNOWN;
    union {
        size_t offset;              // DATA_OFFSET: byte offset into data
        const void* address;        // PC_RELATIVE: resolved virtual address
        int64_t const_value;        // CONST: known integer value
        struct {                    // FORWARDED: callee data pointer
            size_t data_offset;     //   offset into current data to read
            size_t ptr_offset;      //   offset into the loaded struct
        } fwd;
    };
};
```

**`PC_RELATIVE`** is set when an ADRP+ADD sequence resolves to a concrete
address. When a subsequent LDR loads a function pointer from that address,
the walker reads the memory directly and emits `OP_CALL_DIRECT` instead of
`OP_BUDGET` — the target is fully determined at walk time and never needs
to be re-evaluated. This means ADRP resolution requires **no new opcodes**:
the walker does all the work, and the bytecode is simpler (direct calls
where there were previously budgets).

**`CONST`** is set when a register holds a known integer — from `MOVZ`,
`MOVK`, an LDR of a small constant from the closure, or arithmetic on other
CONST registers. This enables condition evaluation (§11.3).

**`FORWARDED`** captures the pattern where X0 at a BL site is derived
from the current data pointer — meaning the callee receives a sub-object
of the closure. This enables interprocedural data propagation (§11.4).

### 11.2 ADRP resolution in the walker

ADRP decoding produces a `PC_RELATIVE` register with the resolved page
address. The subsequent ADD refines it to the exact address. When a BLR
fires on a PC_RELATIVE register, the walker reads the function pointer
from the resolved address at walk time:

```cpp
if (rn < 31 && state.regs[rn].origin == reg_state::PC_RELATIVE) {
    auto target = *reinterpret_cast<const void* const*>(state.regs[rn].address);
    callee = expr::make_call_direct(target);  // fully resolved
}
```

This is the highest-value change and is entirely contained within the
walker — the VM evaluator, bytecode format, and caching are untouched. The
only risk is dereferencing a PC-relative address that points outside the
process's mapped memory. A lightweight guard (checking that the address
falls within a known text or data segment, obtainable via
`dl_iterate_phdr` on Linux or `_dyld_get_image_header` on macOS) would
prevent segfaults from malformed code.

### 11.3 Condition evaluation and path pruning

When a conditional branch depends on a CONST register, the walker can
resolve the branch direction and follow only the taken path:

```cpp
// CBZ Xn, target — branch if Xn == 0
if (match(inst, 0x7F000000, 0x34000000)) {
    uint32_t rn = inst & 0x1F;
    if (rn < 31 && state.regs[rn].origin == reg_state::CONST) {
        // Branch resolved: follow only the taken path.
        if (state.regs[rn].const_value == 0) {
            state.pc = target;  // branch taken
        } else {
            state.pc++;         // fall through
        }
        continue;
    }
    // Unknown register: fork both paths as today.
    ...
}
```

The same pattern applies to CBNZ, TBZ/TBNZ (test a specific bit), and
B.cond (requires tracking the condition flags, which adds complexity — a
reasonable first step is to handle only CBZ/CBNZ and TBZ/TBNZ, which cover
the majority of closure-dependent branches).

Values are loaded from the closure via LDR with a DATA_OFFSET register.
The walker already decodes these loads for pointer tracking; extending the
handler to also set CONST when the loaded width is 32 or 64 bits (and the
loaded value is readable at walk time) is straightforward:

```cpp
// LDR Wt, [Xn, #imm] — 32-bit load, unsigned offset
if (match(inst, 0xFFC00000, 0xB9400000)) {
    uint32_t rt = inst & 0x1F;
    uint32_t rn = (inst >> 5) & 0x1F;
    uint32_t imm = ((inst >> 10) & 0xFFF) * 4;
    if (rn < 31 && state.regs[rn].origin == reg_state::DATA_OFFSET && data) {
        auto addr = static_cast<const char*>(data) + state.regs[rn].offset + imm;
        int32_t val;
        std::memcpy(&val, addr, 4);
        state.regs[rt] = {reg_state::CONST, .const_value = val};
    }
}
```

This is a pure walker change — no new opcodes. Resolved branches simply
don't fork, so the bytecode program has fewer paths and a tighter MAX.

### 11.4 Interprocedural data propagation

This is the one technique that requires a new opcode. Today, `OP_CALL_DIRECT`
enters a callee with `current_data = nullptr`, discarding the data context.
A new opcode carries the data forward:

```cpp
OP_CALL_DIRECT_WITH_DATA = 0x07,  // <target_addr:8> <data_offset:8>
```

The walker emits this when it sees a BL where X0 has DATA_OFFSET provenance
— meaning the callee receives a pointer derived from the current data:

```cpp
// At BL site, check X0 provenance.
if (state.regs[0].origin == reg_state::DATA_OFFSET) {
    callee = expr::make_call_direct_with_data(target, state.regs[0].offset);
} else {
    callee = expr::make_call_direct(target);
}
```

The expression tree and bytecode compiler need a corresponding extension:

```cpp
// New expression kind:
CALL_DIRECT_WITH_DATA  // target + data_offset

// New bytecode emission:
case expr::CALL_DIRECT_WITH_DATA:
    prog.push_back(OP_CALL_DIRECT_WITH_DATA);
    emit_ptr(prog, f.e->target);
    emit_u64(prog, f.e->value);   // data_offset
    stack.pop_back();
    break;
```

The VM evaluator handles it by computing the forwarded data pointer before
entering the callee:

```cpp
case OP_CALL_DIRECT_WITH_DATA: {
    auto addr = read_ptr(ip);
    auto data_off = read_u64(ip);

    // Resolve forwarded data: read the pointer at current_data + offset.
    const void* forwarded_data = nullptr;
    if (current_data) {
        forwarded_data = *reinterpret_cast<const void* const*>(
            static_cast<const char*>(current_data) + data_off);
    }

    // Check eval cache — but only for no-data case.
    // With forwarded data, we must evaluate (same as OP_CALL_INDIRECT today).
    if (!forwarded_data) {
        std::lock_guard<spinlock> lk(g_eval_cache_mu);
        if (auto* r = g_eval_cache.find(addr)) {
            is_exact &= r->is_exact;
            values[vsp++] = r->max_depth;
            break;
        }
    }

    // Cycle detection and callee entry — same as OP_CALL_DIRECT,
    // but set current_data to the forwarded pointer instead of nullptr.
    if (on_stack.contains(addr)) {
        is_exact = false;
        values[vsp++] = opts.indirect_call_budget;
        break;
    }
    prog_store.push_back(get_or_compile(addr, opts));
    call_stack.push_back({ip, end, addr, is_exact, current_data});
    on_stack.insert(addr);
    is_exact = true;
    current_data = forwarded_data;
    ip = prog_store.back().data();
    end = ip + prog_store.back().size();
    break;
}
```

The critical detail is that the forwarded data pointer is a live memory
read: the evaluator dereferences `current_data + data_off` to get the
callee's data pointer, which is itself a pointer into the closure (or a
sub-object thereof). This is safe under the same argument as root-level
closure inspection — the closure is immutable between construction and
execution.

### 11.5 Caching implications

The extended opcodes interact with the caching strategy:

- **ADRP resolution** (§11.2) produces `OP_CALL_DIRECT` with concrete
  targets resolved at walk time. The bytecode program changes per walk
  (different closure → different vtable entries → different targets), but
  the walker is already keyed on function address. The program cache
  (`g_cache`) currently assumes one program per function; with ADRP
  resolution, the same function may compile to different bytecode depending
  on which vtable entries it dispatches through. The simplest fix is to
  **bypass the program cache when the walker resolves any PC_RELATIVE
  targets** — the walk is fast (microseconds) and the payoff is exact
  resolution of previously-inexact calls. A more sophisticated approach
  would key the program cache on `(function, fingerprint_of_resolved_targets)`.

- **`OP_CALL_DIRECT_WITH_DATA`** (§11.4) means the eval result depends on
  the data pointer even for direct calls. The eval cache (`g_eval_cache`)
  is currently keyed on function address alone and only caches results for
  `data=nullptr` evaluations. Data-forwarding calls produce different
  results for different data pointers, so their results cannot be cached
  under the current scheme. The hierarchical eval cache described in
  `docs/stack-analysis-future.md` §11 — keying on `(function,
  target-set fingerprint)` — would address this, but the basic
  implementation can simply skip caching for data-dependent evaluations,
  which is what the current `OP_CALL_INDIRECT` handler already does.

- **Condition evaluation** (§11.3) is entirely a walker-side change and
  produces standard opcodes (fewer paths, tighter MAX). No caching
  implications beyond the program cache point above.

### 11.6 Bootstrapping and BLR-freedom

Any new opcodes must preserve the analyzer's bootstrapping invariant: the
VM code itself must compile to pure inline arithmetic with zero BLR
instructions, so that the analyzer produces exact results when walking its
own code. `OP_CALL_DIRECT_WITH_DATA` is a `switch` case with pointer
arithmetic and `memcpy` — the same pattern as the existing
`OP_CALL_INDIRECT` handler. As long as the new code avoids STL containers
with virtual allocators (which it does — the existing custom `ptr_map`,
`small_ptr_set`, and raw arrays are sufficient), the bootstrapping property
holds.

### 11.7 Incremental adoption

The changes are layered and can be adopted independently:

| Change | Walker | Expression tree | Bytecode | Evaluator |
|--------|--------|----------------|----------|-----------|
| ADRP resolution (§11.2) | New decoder | No change | No change | No change |
| Condition eval (§11.3) | New value tracking | No change | No change | No change |
| Data forwarding (§11.4) | X0 check at BL | New kind | New opcode | New handler |
| Cache refinement (§11.5) | Fingerprint | No change | No change | Sub-cache |

ADRP resolution and condition evaluation are pure walker changes — they
make the expression tree simpler (more CONST/CALL_DIRECT, fewer BUDGET)
without touching the VM. Data forwarding is the only change that threads
through all four layers, and even that is a single new opcode following the
existing `OP_CALL_INDIRECT` pattern.

## 12. Conclusion

The conventional framing of stack depth analysis as a static problem
understates its potential. By operating at spawn time rather than compile
time, the analyzer has access to a wealth of runtime context — constructed
closures, populated vtables, resolved globals, known parameter values —
that can transform conservative estimates into precise bounds.

The current CSP implementation exploits one facet of this context (root-level
closure inspection). The design space extends in several complementary
directions, each independently valuable and incrementally adoptable. The most
impactful improvements — ADRP resolution and interprocedural data flow —
would resolve the majority of currently-inexact indirect calls in real C++
programs, significantly tightening stack bounds without programmer annotation
or runtime profiling.

The deeper lesson is architectural: spawn-time analysis is not a static
analysis that happens to run at runtime. It is a fundamentally different kind
of analysis, one that can read the answers to questions that static analysis
must conservatively approximate. The instruction walker is the vehicle; the
runtime context is the fuel.
