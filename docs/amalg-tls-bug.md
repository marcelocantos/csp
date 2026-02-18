# Amalgamation TLS Caching Bug: Investigation Report

## Problem

When the CSP library's source files are merged into a single-file amalgamation
for distribution, all M:N multi-threading tests crash. The crashes manifest as
segfaults and assertion failures deep in the context-switching code, occurring
non-deterministically but reliably within the first few test cases.

The normal multi-TU build works flawlessly — 298 tests, clean under TSan and
ASan.

## Background

CSP is a C++ microthreading library using Boost.Context (`jump_fcontext` /
`make_fcontext`) for cooperative context switching. In M:N mode, microthreads
are multiplexed across a pool of OS worker threads via a work-stealing
scheduler. A microthread suspended on OS thread A can be resumed on OS thread B.

The library tracks the currently-running microthread via a thread-local
variable:

```cpp
// csp_globals.cpp — definition
namespace csp::detail {
    thread_local Microthread * g_self = nullptr;
}

// csp_internal.h — declaration
namespace csp::detail {
    extern thread_local Microthread * g_self;
}
```

The `start()` function — the entry point for every new microthread — writes
`g_self` twice, with a `switch_to()` call (containing `jump_fcontext`) in
between:

```cpp
static void start(transfer_t t) {
    // ... unpack StartData ...
    auto * self = &sd.self;
    g_self = self;                                  // (1) first write
    auto killyou_val = switch_to(sd.caller, 0);     // warmup handshake
    // MT may now be on a DIFFERENT OS thread
    g_self = self;                                  // (2) second write
    // ...
}
```

Write (1) sets `g_self` on the thread that spawned this microthread. The
`switch_to()` call suspends the new microthread and returns control to the
spawner, which completes the handshake and enqueues the microthread on the
global run queue. When a (potentially different) worker thread picks it up and
resumes it, execution continues after `switch_to()`. Write (2) must set
`g_self` on *that* thread — whichever thread resumed us.

## Investigation

### Narrowing the scope

The crash was narrowed by selectively merging source files. The crash occurs
if and only if `csp_globals.cpp` and `csp.cc` are in the same translation
unit. All other source files can be freely combined without issue.

### Failed hypotheses

1. **`__attribute__((noinline))` on `current_p()`**: No effect. The issue is
   not related to function inlining.

2. **`asm volatile("" ::: "memory")` barrier after `jump_fcontext`**: No
   effect. The compiler memory barrier does not affect TLS address resolution.

3. **TLS aliasing**: A test program confirmed that an `extern thread_local`
   declaration and its definition in the same TU resolve to the same TLS slot
   — no aliasing. The problem is not that two slots exist, but how the single
   slot's *address* is accessed.

### Disassembly comparison

The breakthrough came from comparing the ARM64 disassembly of `start()` between
the separate-TU build and the amalgamated build.

**Separate build** (`csp_globals.cpp` is a different TU):

```asm
; g_self = self;                    (first write)
bl  __ZTWN3csp6detail6g_selfE      ; call TLS wrapper → x0 = &g_self
str x8, [x0]                       ; *x0 = self

; switch_to(sd.caller, 0);         (warmup — may resume on different thread)
bl  __ZN3csp6detailL9switch_toE...

; g_self = self;                    (second write)
bl  __ZTWN3csp6detail6g_selfE      ; call TLS wrapper AGAIN → fresh &g_self
str x8, [x0]                       ; *x0 = self  ✓ correct TLS slot
```

Each `g_self` access calls the opaque TLS wrapper function
`__ZTWN3csp6detail6g_selfE`, which resolves the thread-local address from
scratch on each invocation. After `switch_to()` resumes on a different OS
thread, the second wrapper call returns that thread's `&g_self`.

**Amalgamated build** (`csp_globals.cpp` merged into same TU):

```asm
; g_self = self;                    (first write)
adrp x0, _g_self@TLVPPAGE          ; \ direct TLV descriptor
ldr  x0, [x0, _g_self@TLVPPAGEOFF] ;  } access — inlined
ldr  x9, [x0]                      ;  } resolver function ptr
blr  x9                            ; / call resolver → x0 = &g_self
str  x0, [sp, #0x8]                ; *** CACHE &g_self on stack ***
str  x8, [x0]                      ; *x0 = self  (correct)

; switch_to(sd.caller, 0);         (warmup — may resume on different thread)
bl  __ZN3csp6detailL9switch_toE...

; g_self = self;                    (second write)
ldr  x0, [sp, #0x8]                ; *** RELOAD CACHED &g_self (STALE!) ***
str  x8, [x0]                      ; *x0 = self  ✗ WRONG TLS SLOT
```

## Root Cause

When `csp_globals.cpp` (which *defines* `thread_local Microthread * g_self`) is
in the same TU as `csp.cc`, Clang sees both the `extern` declaration and the
definition. This enables a more aggressive TLS access pattern: instead of
calling the opaque wrapper function, the compiler generates inline TLV
descriptor access (`adrp`/`ldr`/`ldr`/`blr` of the TLV resolver).

Crucially, the compiler then treats the resolved `&g_self` address as a
**stable value within the function** and caches it on the stack
(`str x0, [sp, #0x8]`). After `switch_to()`, it reloads from the cache
(`ldr x0, [sp, #0x8]`) instead of re-resolving TLS.

This is normally a valid optimisation. But `switch_to()` calls
`jump_fcontext`, which saves the entire register file and stack pointer, then
jumps to a different context. The microthread is now suspended. When a
*different* OS thread resumes it, `jump_fcontext` restores the registers and
stack — including the cached `&g_self` from the *original* thread. The second
write to `g_self` goes to the old thread's TLS slot, leaving the current
thread's `g_self` unchanged.

The consequences are immediate and catastrophic: the current thread's `g_self`
still points to its Processor's main microthread (or whoever was last
correctly set). When `do_switch(Status::exit)` reads `g_self` to determine the
`killme` pointer, it gets the wrong microthread, leading to double-frees,
use-after-frees, and assertion failures.

This bug is invisible to standard tooling:

- **TSan/ASan**: Cannot detect it because the write itself is well-formed — it
  just goes to the wrong address.
- **`-O0`**: Still reproduces, because the TLV descriptor inlining and address
  caching is a codegen-level decision, not an optimiser pass.
- **Compiler barriers**: `asm volatile("" ::: "memory")` forces the compiler
  to reload *values* from memory, but the cached *address* is treated as a
  local variable, not a memory load.

The wrapper-call pattern used in the separate-TU build is immune because the
wrapper is an opaque external function — the compiler cannot prove that the
returned address is stable, so it must call it again after any function call
that might have side effects.

## Resolution

The fix is to keep `csp_globals.cpp` as a separate translation unit in the
amalgamated output. The amalgamation script (`scripts/amalgamate.py`) was
modified to emit four files instead of three:

| File | Contents |
|---|---|
| `amalg/csp.h` | All headers under `include/csp/` (except `part/`) |
| `amalg/csp.cpp` | All source files **except** `csp_globals.cpp`, plus fcontext inline asm |
| `amalg/csp_globals.cpp` | Only `csp_globals.cpp` (thread-local definitions) |
| `amalg/csp_parts.h` | All headers under `include/csp/part/` |

Users of the amalgamation must compile both `csp.cpp` and `csp_globals.cpp` as
separate compilation units. The comment in the script explains why:

```python
# csp_globals.cpp MUST be a separate TU.  It defines the
# thread_local variable g_self.  When the definition and the
# extern declaration (from csp_internal.h) are in the same TU,
# Clang generates direct TLV-descriptor access and may cache the
# resolved TLS address across jump_fcontext calls.  Because
# jump_fcontext can resume a microthread on a different OS thread,
# the cached address becomes stale and writes to the wrong TLS
# slot — corrupting g_self and crashing the M:N scheduler.
# Keeping csp_globals.cpp separate forces the wrapper-call
# pattern, which re-resolves the TLS address on every access.
```

## Verification

- Normal build: 298/298 tests pass (unchanged).
- Amalgamated build with separate `csp_globals.cpp`: M:N tests pass
  20/20 consecutive iterations (23 test cases, 166 assertions each).
  Previously crashed on the first iteration every time.
