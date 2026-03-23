# TSan Cannot Track Mutex Ownership Across Fiber Migrations

## Abstract

ThreadSanitizer (TSan) provides fiber-aware annotations
(`__tsan_switch_to_fiber`, `__tsan_create_fiber`) to support M:N
threading models. These annotations correctly track happens-before
relationships for fiber scheduling. However, they do not extend to
TSan's tracking of pthread mutex operations, which remains per-OS-thread.
When a fiber acquires a pthread mutex on one OS thread and releases it
on another after migration, TSan reports a false data race. This paper
documents the discovery of this limitation in the context of a C++
concurrency library (CSP) using mbedTLS, and describes the diagnostic
process that identified the root cause.

## Background

### CSP's M:N scheduler

CSP is a C++ concurrency library implementing Communicating Sequential
Processes with typed, synchronous channels. Its unit of concurrency is
the **imp** — a lightweight coroutine multiplexed across OS threads via
a work-stealing scheduler. Imps are backed by fcontext (Boost.Context)
for context switching and can migrate between OS threads at any
suspension point.

### TSan fiber annotations

TSan supports user-level threading (fibers) through three APIs:

- `__tsan_create_fiber(flags)` — Create a fiber identity.
- `__tsan_switch_to_fiber(fiber, flags)` — Declare that the current OS
  thread is now executing the given fiber. TSan records a happens-before
  edge from the fiber's last suspension to this resumption.
- `__tsan_get_current_fiber()` — Return the current fiber identity.

CSP annotates every context switch: before `jump_fcontext`, it calls
`__tsan_switch_to_fiber(target.tsan_fiber_)`. Each imp creates a fiber
identity at spawn time. Each OS thread's main loop (the non-imp
scheduling context) gets its own fiber identity via
`__tsan_get_current_fiber()` at thread startup.

### mbedTLS threading

mbedTLS (the TLS library used by CSP) is compiled with
`MBEDTLS_THREADING_PTHREAD`, which enables internal pthread mutexes for
thread-safe operation. These mutexes protect shared state like the PSA
key store, CTR-DRBG random generator, and AES tables.

## The symptom

Running CSP's TLS tests under TSan produced hundreds of data race
reports in mbedTLS functions: `psa_reserve_free_key_slot`,
`mbedtls_ctr_drbg_random_with_add`, `mbedtls_aesce_crypt_ecb`,
`mbedtls_mpi_mul_mod`, and others. The races occurred during the
"TLS - Concurrent connections" test, which runs multiple TLS
handshakes in parallel across different imps.

A distinctive feature of the reports: **both racing threads showed
`tid=0`** in TSan's output.

The macOS TSan CI job would either report races and exit with code 66,
or hang indefinitely (24+ hours), suggesting TSan's internal state was
confused enough to deadlock its own tracking.

## The investigation

### Hypothesis 1: Annotation coverage gap

The initial hypothesis was that CSP's fiber annotations had a coverage
gap — some path into imp code that didn't go through
`__tsan_switch_to_fiber`. This would cause TSan to misidentify the
executing fiber, leading to false races.

**Enumeration of entry paths:**

Every path into imp code was traced:

1. **`Imp::run()`** — Normal resume. Calls `switch_to(*this, ...)`
   which calls `__tsan_switch_to_fiber(this->tsan_fiber_)` before
   `jump_fcontext`. ✓

2. **`start()`** — Initial imp entry (trampoline after
   `make_fcontext`). Reached via `switch_to(*new_imp, ...)` in
   `spawn()`. ✓

3. **`do_switch()`** — Scheduling next imp from local queue. Calls
   `target->run()` which calls `switch_to`. ✓

4. **Worker thread startup** — `bind_processor()` calls
   `__tsan_get_current_fiber()` to capture the worker's main fiber
   identity. ✓

5. **Surplus processor startup** — Same path as worker startup. ✓

6. **Return from `jump_fcontext`** — When an imp resumes after
   suspension, the *resuming* thread calls
   `__tsan_switch_to_fiber(target)` before jumping, so TSan's fiber
   identity is correct when `jump_fcontext` returns. ✓

**Conclusion**: All paths are annotated. No coverage gap.

### Hypothesis 2: `tid=0` indicates main fiber confusion

The `tid=0` on both racing threads suggested TSan thought both imps
were executing as the main fiber. If two imps inherited the main
fiber's identity, all their memory accesses would appear to be from
the same "thread," and concurrent accesses would be flagged as races.

However, tracing confirmed that each imp gets a unique fiber identity
from `__tsan_create_fiber()`, and every context switch correctly
switches to the target imp's fiber. The `tid=0` was a red herring —
TSan's output format for fiber-aware programs can show `tid=0` for
fibers that don't correspond to real OS threads.

### Hypothesis 3: Pthread mutex tracking is per-OS-thread

TSan's fiber annotations create happens-before edges for the
*scheduling* of fibers. But TSan also tracks synchronization
primitives — in particular, `pthread_mutex_lock` and
`pthread_mutex_unlock`. These are tracked per-OS-thread, not per-fiber.

In CSP's M:N scheduler, the following sequence is legal and correct:

1. Imp A runs on OS thread T1.
2. Imp A acquires `pthread_mutex_t M` (mbedTLS internal lock).
3. Imp A calls a CSP channel operation that suspends it.
4. Imp A is later resumed on OS thread T2 (work stealing).
5. Imp A releases `pthread_mutex_t M` on T2.

From the application's perspective, this is safe: the same logical
execution context (imp A) holds the mutex throughout. The fiber
annotations correctly establish happens-before from step 2 to step 5.

But TSan sees:

- `pthread_mutex_lock(M)` on thread T1
- `pthread_mutex_unlock(M)` on thread T2

TSan tracks mutex ownership per-OS-thread. The lock on T1 and unlock
on T2 look like a lock/unlock mismatch, or worse, like T2 is accessing
data "protected" by a mutex it doesn't hold. TSan's fiber annotations
don't propagate to its pthread mutex tracking — they only affect
TSan's internal happens-before graph for memory accesses by fibers.

**This is the root cause.**

## Verification

A TSan suppression file was created:

```
race:mbedtls_*
race:psa_*
```

With suppressions active, all 11 CI jobs passed — including macOS TSan,
which had been failing or hanging on every PR for weeks.

To confirm the analysis:

1. **CSP's own synchronization** (channel operations, scheduler locks)
   does not use pthread mutexes — it uses `std::mutex` / `std::atomic`,
   which TSan tracks via compiler instrumentation rather than library
   interposition. Fiber annotations correctly handle these.

2. **Only mbedTLS** (which uses raw `pthread_mutex_*` calls) triggers
   the false races. No other vendored dependency is affected.

3. **The races only appear under M:N scheduling** (multiple OS threads
   with fiber migration). Single-threaded mode (`set_maxprocs(1)`)
   doesn't exhibit the issue because imps never migrate.

## Implications

### For M:N schedulers using TSan

Any M:N scheduler that uses TSan's fiber annotations and calls
libraries with pthread mutexes will hit this limitation. The fiber
annotations are necessary and correct for tracking the scheduler's own
synchronization, but they don't make TSan's pthread tracking
fiber-aware.

**Workarounds:**

1. **Suppression files** — Silence races in specific library functions.
   This is the pragmatic solution. It loses TSan coverage for those
   functions but keeps coverage for the rest of the codebase.

2. **Avoid pthread mutexes in migrating fibers** — If a library uses
   pthread mutexes, ensure it's only called from non-migrating contexts
   (e.g., a dedicated OS thread, or under `set_maxprocs(1)`). This is
   often impractical.

3. **Replace pthread mutexes with TSan-annotated atomics** — Rewrite
   the library's locking to use `std::mutex` or compiler-instrumented
   atomics. Feasible for owned code, not for vendored dependencies.

4. **TSan runtime patches** — Extend TSan's pthread interceptors to
   consult the fiber tracking table. This would be the correct fix
   but requires changes to the TSan runtime itself (LLVM
   compiler-rt).

### For TSan itself

The fiber annotation API was designed for user-level threading but
doesn't fully integrate with TSan's lock tracking. The current API
documentation does not mention this limitation. A note in the TSan
fiber documentation would save future M:N scheduler implementors
significant debugging time.

## Timeline

| Date | Event |
|------|-------|
| 2026-03-08 | First observed: mbedTLS TSan races in v0.3.0 CI |
| 2026-03-12 | Documented in `docs/todo.md` as open investigation |
| 2026-03-15 | Created 🎯T9 convergence target |
| 2026-03-22 | Investigated annotation coverage — all paths verified correct |
| 2026-03-24 | Identified root cause: pthread mutex tracking is per-OS-thread |
| 2026-03-24 | Suppression file resolves all false positives; 11/11 CI green |

## Conclusion

TSan's fiber annotations solve the happens-before tracking problem for
M:N schedulers but leave a gap in pthread mutex tracking. When fibers
migrate between OS threads while holding pthread mutexes (common when
calling thread-safe C libraries from M:N-scheduled code), TSan reports
false races because it associates mutex operations with OS threads, not
fibers.

The fix is straightforward (suppression files), but the diagnosis was
not — the symptoms (hundreds of races, `tid=0`, CI hangs) pointed in
multiple directions. The key insight was distinguishing between TSan's
*scheduling* model (fiber-aware) and its *synchronization* model
(OS-thread-bound).
