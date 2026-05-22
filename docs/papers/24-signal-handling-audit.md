# 24 — Signal Handling Audit: Async-Signal-Safety

## Scope

This paper audits every signal handler installed by the CSP library for
compliance with POSIX async-signal-safety rules.

POSIX defines a set of functions that may safely be called from a signal
handler (the "async-signal-safe" list). In brief:

- **Safe**: `write`, `read`, `_exit`, `kill`, `signal`, `sigaction`,
  `sigemptyset`, `sigfillset`, `sigaddset`, `sigdelset`,
  `dup`/`dup2`, `open`/`close`/`pipe`, `raise`, `abort`, and operations on
  `sig_atomic_t` / `volatile sig_atomic_t`.
- **Safe with lock-free atomics** (C++11 / POSIX.1-2024): C++ atomic
  load/store on types that advertise `is_lock_free() == true` are safe because
  they compile down to single machine instructions with no OS calls.
- **Not safe**: `malloc`/`free`, `printf`/`fprintf`, `pthread_mutex_lock`,
  most STL containers, anything that sets `errno` without restoration,
  `longjmp`, C++ exceptions, memory allocation operators.

---

## 1. Handlers Found

### Handler A — `sig_handler` (POSIX signals, user-defined)

| Attribute | Value |
|---|---|
| Location | `src/signal.cc`, line 43 |
| Installed by | `install_handler()`, called from `csp::signal::notify()` |
| Signals | Any user-specified signal number 1–63 |
| Install mechanism | `sigaction(sig, &sa, &g_old_actions[sig])` with `SA_RESTART` flag |

**Operations performed**:

1. Read `sig` argument — signal number passed by the kernel (safe; stack only).
2. Range check `sig < 0 || sig > MAX_SIGNO` — comparison, no syscall (safe).
3. Integer arithmetic to build `bit` and `byte` — safe.
4. `g_sig_pipe_count.load(std::memory_order_acquire)` — lock-free atomic
   load on `std::atomic<int>` (safe; compiles to a single load + barrier).
5. Loop over at most `MAX_SIG_PIPES` (64) entries:
   - `g_sig_pipes[i].sig_mask.load(std::memory_order_acquire)` — lock-free
     atomic load on `std::atomic<uint64_t>` (safe).
   - `::write(g_sig_pipes[i].write_fd, &byte, 1)` — POSIX async-signal-safe
     syscall (safe).
6. **`errno` save/restore** — `write()` may set `errno`; the handler saves
   `errno` on entry and restores it on exit (see violation V1 below).

**Global data accessed** (read-only from the handler's perspective):

- `g_sig_pipes[]` — static array of `{int write_fd; atomic<uint64_t> sig_mask}`.
  `write_fd` is an `int`; reads are naturally atomic on all relevant
  architectures. The array itself is sized at compile time and never
  reallocated.
- `g_sig_pipe_count` — `std::atomic<int>`, lock-free.

**Mutexes touched**: none. `g_sig_mu` is declared in the same TU but the
handler never acquires it (only `notify()` does, outside of the handler).

---

### Handler B — `SIG_IGN` on `SIGPIPE`

| Attribute | Value |
|---|---|
| Location | `src/runtime.cpp`, line 46 |
| Installed by | `Runtime::init()` at runtime startup |
| Signal | `SIGPIPE` |
| Install mechanism | `::signal(SIGPIPE, SIG_IGN)` |

`SIG_IGN` is the kernel's own disposition — there is no user function invoked.
No async-signal-safety question arises.

---

## 2. Violations Found

### V1 — Missing `errno` save/restore in `sig_handler` (FIXED)

**Severity**: Low-to-medium. Observable as corruption of `errno` in
interrupted code.

**Details**: The `write()` call on line 49 of the original `signal.cc` is
async-signal-safe as a syscall, but it may modify `errno` (e.g., setting
`EAGAIN` when the pipe is full in non-blocking mode). If the signal interrupts
code that is about to inspect `errno` after a syscall, the interrupted code
will see the handler's side-effect instead of its own error code.

POSIX (IEEE Std 1003.1, "Signal Concepts") explicitly requires:

> "If any signal is caught during the execution of a signal catching function,
> the `errno` value is saved and restored."

and more precisely, the async-signal-safe function list requires that a
handler not change the value of `errno` as seen by the interrupted code.

**Fix** (committed in this branch): Added `#include <cerrno>` and save/restore
`errno` around the entire handler body:

```cpp
void sig_handler(int sig) {
    int saved_errno = errno;
    if (sig < 0 || sig > MAX_SIGNO) { errno = saved_errno; return; }
    // ... atomic loads + write() calls ...
    errno = saved_errno;
}
```

---

## 3. Items Reviewed and Found Safe

### 3.1 Lock-free atomics in the handler

`g_sig_pipe_count` (`atomic<int>`) and `g_sig_pipes[i].sig_mask`
(`atomic<uint64_t>`) use `load(memory_order_acquire)`. Both are lock-free on
all supported platforms (x86-64, arm64). Lock-free atomic operations are
async-signal-safe: they compile to native load/fence instructions with no OS
calls and no internal mutexes.

The publish side (in `notify()`, under `g_sig_mu`) stores the mask with
`memory_order_relaxed` and then stores the count with `memory_order_release`.
The handler acquires on the count first, then acquires on the mask. This is a
correct release/acquire pair: seeing count `>= i+1` guarantees the mask for
slot `i` is visible.

### 3.2 `write_fd` read without atomics

`g_sig_pipes[i].write_fd` is a plain `int`, not atomic. The handler reads it;
`notify()` sets it before publishing the count. The release store on
`g_sig_pipe_count` in `notify()` ensures the handler sees the correct `write_fd`
before it can read it (the count acts as a gatekeeper). Once published, the fd
is only modified by the sentinel imp (zeroing it after masking the signal) —
and by that point the mask has been cleared to zero with `memory_order_release`,
which the handler sees before it can attempt a write. Safe.

### 3.3 `g_sig_mu` not used in the handler

`g_sig_mu` is a `std::mutex` held by `notify()` during pipe registration.
It is never acquired inside `sig_handler`. No deadlock risk.

### 3.4 Non-blocking pipe write

The write end of each pipe is set non-blocking (`io::set_nonblock`). A write
that would block returns `EAGAIN` instead. Signal coalescing (dropping a signal
notification when the pipe is full) is acceptable: the pipe has a kernel buffer
(typically 64 KB), and dropping a notification during pathological burst
conditions is preferable to blocking or deadlocking the handler. The handler
silently discards the `EAGAIN` return, which is correct after the `errno`
fix (the fix does not alter the discard logic, only protects the caller's
`errno`).

### 3.5 `F_SETNOSIGPIPE` / `SIG_IGN` on `SIGPIPE`

On macOS, `F_SETNOSIGPIPE` suppresses `SIGPIPE` delivery for the specific
write fd. On Linux, `SIG_IGN` on `SIGPIPE` is set process-wide by
`Runtime::init()`. Either mechanism prevents `write()` inside the handler from
recursively delivering `SIGPIPE` to itself — which would be a re-entrant
signal-handler hazard if `SIGPIPE` were also under the user's `notify()` set.

### 3.6 `SigPipeCleanup` destructor

`SigPipeCleanup::~SigPipeCleanup()` runs at static-destructor time. The
comment notes that by that point all imps are dead and no signal handler can
race. This is correct for POSIX: static destructors run after `main()` returns,
after all threads have been joined (or detached and dead), and after any
`atexit()` handlers. Signal handlers can still be delivered, but the cleanup
uses `g_sig_pipe_count` (relaxed load, acceptable since no concurrent imp
modifies it at this point) and calls `::close()`. If a handler fires during
`close()`, it will attempt `write()` to a closing fd, which returns `EBADF`
(harmless and now captured by the `errno` fix).

---

## 4. Standing Invariants for Future Signal-Handling Code

The following rules apply to any future signal handler added to the CSP library:

1. **Only async-signal-safe syscalls inside handlers.** The POSIX list is the
   authority. Permitted: `write`, `read`, `_exit`, `kill`, `raise`, `dup`,
   `dup2`, `open`, `close`, `pipe`, `sigaction`, `sigemptyset`, `sigfillset`,
   `sigaddset`, `sigdelset`.

2. **Always save and restore `errno`.** Any handler that calls a function that
   may set `errno` must save it on entry and restore it on exit. This includes
   any syscall wrapper, even ones declared async-signal-safe.

3. **No mutexes in handlers.** A handler that acquires a mutex can deadlock if
   the signal interrupts a thread that already holds that mutex. Use lock-free
   atomics or pre-allocated static data instead.

4. **Publish via release/acquire on a lock-free atomic count or flag.** Data
   that the handler reads must be visible before the handler can access it.
   Use a release store on the count/flag in the normal path; use acquire load
   in the handler. Plain `int` fields may be used if they are published before
   the atomic count/flag that gates the handler's access.

5. **Self-pipe is the preferred delivery mechanism.** Handlers write a single
   byte to a pre-allocated non-blocking pipe; imps drain the pipe. Complex
   logic (allocation, channel operations, STL) belongs in the imp, not the
   handler.

6. **Document explicitly.** Each handler must have a block comment listing:
   every syscall it makes, every global it accesses, and the reason each is
   safe.
