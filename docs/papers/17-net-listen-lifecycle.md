# net::listen Accept Loop Lifecycle — Investigation

## Status: RESOLVED

Both bugs from the original investigation are now fixed (668a011).

## Bug 1: HAMT leak (SOLVED earlier)

Shared `cancel_guard` across imps causes `csp::local` to restore in
the wrong imp's dynamic scope. TLA+ spec `formal/ListenLifecycle.tla`
confirms. Fix: stopper-imp pattern where producer owns the guard
exclusively.

## Bug 2: Echo test failure (SOLVED)

### Root cause

Two independent bugs combined to produce the failure (3W+1R channel
leak + `std::terminate`):

**2a. TCP half-close via dup(fd)**

`make_connection()` calls `dup(fd)` to give `byte_reader` and
`byte_writer` separate file descriptors for the same socket. When
`byte_writer` exits and calls `close(wfd)`, only the dup'd fd is
closed — the underlying socket stays open because `rfd` still holds
a reference. No TCP FIN is sent, so the peer's `byte_reader` never
sees EOF.

This causes a deadlock: after the echo roundtrip completes, the client
drops `conn.output` (killing byte_writer), but the server's
byte_reader keeps waiting for data that will never arrive. The server's
echo loop never exits, so the listen infrastructure (sentinel, stopper,
producer) never shuts down.

**Fix**: Socket-aware byte_writer in `make_connection()` calls
`::shutdown(wfd, SHUT_WR)` before `close(wfd)`. `shutdown()` operates
on the underlying socket regardless of fd refcount, properly sending
FIN.

**2b. Test harness incompatibility with M:N scheduler**

The net tests used `RunStats`, which spawns an exception handler imp
that blocks forever on `r >> ex`. In M:N mode, `schedule()` calls
`main_loop()` which waits for `live_gs == 0`. The exception handler
keeps `live_gs > 0`, so `schedule()` never returns.

Additionally, the tests lacked `shutdown_runtime()` / `set_maxprocs()`
calls. Without these, the first `spawn()` triggers runtime init with
hardware concurrency. But the `RunStats` exception handler imp was
already in the system, and in single-P mode (when hardware_concurrency
resolved to 1 in some configurations), the `main_loop_scheduler`
deadlock detection would fire prematurely — before `io::resolve()`
(which uses the blocking pool) could complete.

**Fix**: Match the pattern used by `io.test.cc` — explicit
`shutdown_runtime()` / `set_maxprocs(2)` before the test,
`shutdown_runtime()` after, no `RunStats`.

### Why the original hypothesis was wrong

The investigation plan (steps 1–7) assumed the bug was in fcontext
lifecycle management — the `std::terminate` with no current exception
pointed to a context-switch issue. In reality:

- The `std::terminate` was a secondary effect: the scheduler exited
  early (deadlock detection), leaving imps alive. During process exit,
  static destructors encountered inconsistent state.
- The 3W+1R channel leak was from imps that never ran to completion
  (abandoned by the premature scheduler exit).
- The "fcontext terminate" was never a fcontext bug — it was the
  process crashing during cleanup of leaked imps.

### Remaining design issues (not bugs)

1. **`RunStats` + M:N `main_loop`**: `RunStats` spawns a permanent
   exception handler imp, making it incompatible with `main_loop()`
   which waits for `live_gs == 0`. I/O tests work around this by not
   using `RunStats`. A cleaner fix would be to exclude the exception
   handler from `live_gs`, or have `main_loop` support "background"
   imps.

2. **Single-P deadlock detection vs blocking pool**: The
   `main_loop_scheduler` deadlock check (`!has_pending_signals &&
   !has_global_work`) doesn't account for outstanding blocking pool
   work. An imp suspended in `csp::blocking()` has no reactor signal
   and no global work yet — the blocking pool thread hasn't finished.
   The scheduler can exit before the blocking pool completes. This
   needs a `pending_blocking_` counter similar to `pending_signals_`.
