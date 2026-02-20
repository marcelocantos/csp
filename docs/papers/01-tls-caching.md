# When Context Switching Breaks Your Compiler

## Abstract

We describe a bug in which Clang's thread-local storage code
generation, combined with userspace context switching via
`jump_fcontext`, causes a C++ imp-based concurrency library to crash — but
only when the source files are compiled as a single translation unit.
The compiler caches a resolved TLS address on the stack; a context
switch resumes the imp on a different OS thread, where the
cached address points to the wrong thread's TLS slot. The bug is
invisible to sanitizers, unaffected by optimisation level, and immune
to compiler memory barriers. We explain the root cause via ARM64
disassembly, present the fix, and discuss the broader implications
for any library that performs userspace context switching.

## 1. The symptom

CSP is a C++ imp-based concurrency library. It multiplexes lightweight
userspace threads (imps) across a pool of OS worker threads
using an M:N scheduler. Each worker runs a loop: pick an imp
from the local run queue, context-switch into it, and when it
suspends, pick the next one. Imps can migrate between workers
— an imp suspended on OS thread A may be resumed on OS
thread B.

For distribution, the library ships as three files: a single header
(`csp.h`), an implementation file (`csp.cpp`), and a small file
containing thread-local variable definitions (`csp_globals.cpp`). The
three-file structure is generated from a conventional multi-file
development tree by an amalgamation script.

During development of the amalgamation system, we discovered that
merging `csp_globals.cpp` into `csp.cpp` — producing two files
instead of three — caused every M:N threading test to crash.
Segfaults and assertion failures deep in the context-switching code,
occurring non-deterministically but reliably within the first few test
cases.

The normal multi-file build worked flawlessly: 298 tests, clean under
both ThreadSanitizer and AddressSanitizer.

## 2. Background: TLS and context switching

The library tracks the currently-running imp via a
thread-local variable:

```cpp
// csp_globals.cpp — definition
namespace csp::detail {
    thread_local Imp * g_imp = nullptr;
}

// csp_internal.h — declaration
namespace csp::detail {
    extern thread_local Imp * g_imp;
}
```

The `start()` function — the entry point for every new imp —
writes `g_imp` twice, with a context switch in between:

```cpp
static void start(transfer_t t) {
    auto * self = &sd.self;
    g_imp = self;                            // (1) set g_imp on spawner's thread
    auto killyou_val = switch_to(sd.caller, 0);  // warmup handshake
    // *** may now be on a DIFFERENT OS thread ***
    g_imp = self;                            // (2) set g_imp on resume thread
    // ...
}
```

Write (1) sets `g_imp` on the thread that created the imp.
The `switch_to()` call suspends it and returns control to the
spawner, which enqueues the imp on the global run queue. When
some worker thread — possibly a different one — picks it up and
resumes it, execution continues after `switch_to()`. Write (2) must
set `g_imp` on *that* thread, whichever it is.

The `switch_to()` function calls `jump_fcontext` from Boost.Context,
which saves the entire CPU register file and stack pointer to memory,
then loads a different register set and stack pointer, transferring
control to a different context. The suspended imp's stack
frame is preserved exactly as it was.

## 3. The investigation

### 3.1 Narrowing the scope

By selectively merging source files, we determined that the crash
occurs if and only if `csp_globals.cpp` and `csp.cc` are in the same
translation unit. All other source files can be freely combined
without issue. This pointed to something specific about the
interaction between the thread-local *definition* and its use site.

### 3.2 Failed hypotheses

Three attempted fixes gave no improvement:

1. **`__attribute__((noinline))` on accessor functions.** The crash
   is not caused by function inlining — it persists regardless of
   inlining decisions.

2. **`asm volatile("" ::: "memory")` after `jump_fcontext`.** A
   compiler memory barrier forces the compiler to reload *values*
   from memory, but it does not force re-resolution of TLS addresses.
   The barrier has no effect.

3. **TLS aliasing.** A test confirmed that an `extern thread_local`
   declaration and its definition in the same TU resolve to the same
   slot — there is no second slot. The problem is not *which* slot is
   accessed but *how* the slot's address is obtained.

### 3.3 The disassembly breakthrough

The answer became clear only by comparing the ARM64 disassembly of
`start()` between the two-TU build and the single-TU build.

**Separate translation units** (correct):

```asm
; g_imp = self;                    (first write)
bl  __ZTWN3csp6detail6g_impE      ; call TLS wrapper → x0 = &g_imp
str x8, [x0]                       ; *x0 = self

; switch_to(sd.caller, 0);         (may resume on different thread)
bl  __ZN3csp6detailL9switch_toE...

; g_imp = self;                    (second write)
bl  __ZTWN3csp6detail6g_impE      ; call TLS wrapper AGAIN → fresh &g_imp
str x8, [x0]                       ; *x0 = self  ✓ correct
```

Each `g_imp` access calls the TLS wrapper function, which resolves
the thread-local address from scratch. After `switch_to()` resumes
on a different OS thread, the second wrapper call returns *that*
thread's `&g_imp`.

**Single translation unit** (broken):

```asm
; g_imp = self;                    (first write)
adrp x0, _g_imp@TLVPPAGE          ;  ┐ direct TLV descriptor
ldr  x0, [x0, _g_imp@TLVPPAGEOFF] ;  │ access — inlined by
ldr  x9, [x0]                      ;  │ compiler, no wrapper call
blr  x9                            ;  ┘ call TLV resolver → x0 = &g_imp
str  x0, [sp, #0x8]                ; *** CACHE &g_imp on stack ***
str  x8, [x0]                      ; *x0 = self  (correct on this thread)

; switch_to(sd.caller, 0);         (may resume on different thread)
bl  __ZN3csp6detailL9switch_toE...

; g_imp = self;                    (second write)
ldr  x0, [sp, #0x8]                ; *** RELOAD CACHED &g_imp ***
str  x8, [x0]                      ; *x0 = self  ✗ WRONG TLS SLOT
```

## 4. Root cause

When the `extern thread_local` declaration and the `thread_local`
definition appear in the same translation unit, Clang has complete
visibility into the TLS variable. It generates inline TLV descriptor
access — loading the resolver function pointer from the TLV page
and calling it directly — rather than calling the opaque wrapper
function.

This is faster. But it also gives the compiler confidence that the
*address* returned by the resolver is stable within the function. The
compiler treats the resolved `&g_imp` as a loop-invariant local and
caches it on the stack (`str x0, [sp, #0x8]`). After `switch_to()`,
it reloads from the cache (`ldr x0, [sp, #0x8]`) instead of
re-resolving.

Normally this is valid. TLS addresses *are* stable — for the
lifetime of the OS thread. But `switch_to()` calls `jump_fcontext`,
which saves the registers and stack and jumps to a different context.
The imp is now suspended, its stack frozen. When a different
OS thread resumes it, `jump_fcontext` restores the frozen stack —
including the cached `&g_imp` from the original thread. The second
write to `g_imp` goes to the old thread's TLS slot, leaving the
resuming thread's `g_imp` untouched.

The consequences are immediate. The resuming thread's `g_imp`
still points to whatever imp it was running before — or to
the processor's sentinel. When the scheduler reads `g_imp` to
determine which imp to clean up or reschedule, it operates on
the wrong one. Double-frees, use-after-frees, and assertion failures
follow within a few scheduling cycles.

## 5. Why standard tooling cannot help

This bug occupies a blind spot shared by every standard debugging
tool:

- **ThreadSanitizer / AddressSanitizer**: The write to `g_imp` is
  well-formed. The pointer dereference is valid. The memory is
  allocated and writable. The problem is that it's the *wrong*
  valid address — a different thread's TLS slot. Sanitizers track
  which thread accesses which memory, but both threads are
  legitimately accessing their own TLS regions.

- **Optimisation level (`-O0`)**: The TLV descriptor inlining and
  address caching are code-generation decisions, not optimiser
  passes. They occur even at `-O0` because they're part of how Clang
  emits TLS accesses when it has full visibility. Compiling with
  `-O0` does not change the code path.

- **Compiler memory barriers**: `asm volatile("" ::: "memory")`
  tells the compiler that memory contents may have changed, forcing
  it to reload *values*. But the cached TLS address is stored in a
  stack slot and treated as a local variable, not a memory location
  that could be modified by external code. The barrier does not
  invalidate it.

- **Volatile**: Marking `g_imp` as `volatile` forces re-reading
  the *value* of `g_imp` on every access, but does not force
  re-resolution of the *address* of `g_imp`. The compiler still
  caches the address.

The only diagnostic that revealed the bug was direct comparison of
disassembly output between the working and broken builds. There is
no language-level mechanism to tell the compiler "the TLS address may
be invalid after this call." The compiler's assumption — that a
thread cannot change which OS thread it is running on — is baked
into the code generation model.

## 6. The fix

The fix is architectural: keep `csp_globals.cpp` as a separate
translation unit. When the definition of `g_imp` is in a different
TU, the compiler sees only the `extern thread_local` declaration. It
cannot resolve the TLV descriptor at compile time and must call the
opaque TLS wrapper function on every access. The wrapper re-resolves
the TLS address from scratch each time. After a context switch to a
different OS thread, the wrapper returns the new thread's `&g_imp`.

The distribution now ships as three files instead of two:

| File | Contents |
|---|---|
| `csp.h` | All public headers |
| `csp.cpp` | All implementation + fcontext inline assembly |
| `csp_globals.cpp` | Thread-local definitions only |

The cost is one extra compilation unit. The benefit is that the
compiler can never cache a TLS address across a context switch.

## 7. Verification

The normal multi-file build was unaffected (298 tests passing as
before). The distribution build with `csp_globals.cpp` as a separate
TU was run 20 consecutive times, each executing the full M:N test
suite (23 test cases, 166 assertions each). All 20 iterations passed.
With the merged single-TU build, the suite had crashed on the first
iteration every time.

## 8. Broader implications

This is not a Clang bug. The compiler's optimisation is correct under
the C++ memory model, which assumes that a thread's identity does not
change during execution. The Itanium C++ ABI, the ARM64 ABI, and the
x86-64 System V ABI all define TLS resolution mechanisms that are
permitted to cache results — the spec language is "the implementation
may assume that the TLS block address is constant within a thread."

Any library that performs userspace context switching —
`jump_fcontext`, `swapcontext`, `setjmp`/`longjmp`-based
coroutines, or hand-written assembly — is potentially vulnerable to
equivalent bugs on any platform where the compiler can see both the
TLS definition and its use. The specific manifestation depends on the
compiler, the target architecture, and the TLS access model
(local-exec, initial-exec, general-dynamic), but the underlying
issue is the same: **the compiler assumes threads don't migrate
between OS threads, because in standard C++, they can't.**

Userspace threading libraries violate this assumption by design.
The C++ standard provides no mechanism to inform the compiler that
a function call may change the thread identity of the caller. Until
it does, the only portable defence is to ensure that TLS definitions
and their use sites are in separate translation units, forcing the
compiler to treat TLS resolution as an opaque external call.

This has implications for:

- **Go's runtime**: Go's goroutine scheduler performs M:N
  multiplexing with `runtime.goexit` and `runtime.mcall`. The
  runtime accesses TLS via assembly, not C compiler-generated code,
  sidestepping this issue — but any cgo code that touches
  `thread_local` C variables could be vulnerable.

- **Rust's async runtimes** (Tokio, async-std): These use
  OS-level threads, not userspace context switching, so TLS remains
  consistent. However, Rust's `#[thread_local]` attribute on
  nightly enables compiler-inlined TLS access, and any future
  stackful coroutine implementation would face the same issue.

- **Boost.Fiber, libaco, minicoro**: Any stackful coroutine
  library that migrates fibers between OS threads. Libraries that
  pin fibers to a single thread (the more common design) are not
  affected.

- **Amalgamation-style distributions** (SQLite, stb libraries):
  The single-TU compilation model is popular for ease of
  integration. Any library using both `thread_local` and userspace
  context switching must carefully partition its source files to
  prevent TLS address caching.

The fix is simple once you know the cause. The hard part is
diagnosing it: the symptoms (random crashes, assertion failures)
give no hint that TLS code generation is involved, and every
standard debugging tool reports the code as correct. The lesson is
that userspace context switching operates below the abstraction level
that C++ compilers reason about, and the seams show in unexpected
places.
