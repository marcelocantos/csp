# Blocking Reference

Offload blocking or long-running work to an OS thread pool without stalling
the microthread scheduler.

---

## Table of Contents

1. [blocking](#blocking) -- run a callable on the blocking thread pool

---

## blocking

Run a callable on an OS thread pool, suspending the calling microthread until
it completes.

### Signature

```cpp
template <typename Fn>
auto blocking(Fn&& fn) -> std::invoke_result_t<Fn>;
```

**Header:** `#include "csp/blocking.h"`

### Description

`blocking` offloads a callable `fn` to a pool of OS threads managed by the
runtime, suspending the calling microthread until `fn` returns. The microthread
is detached from its processor while `fn` executes, freeing the processor to
run other microthreads. When `fn` completes, the microthread is rescheduled and
resumes with the return value of `fn`.

If `fn` returns `void`, `blocking` returns `void`. Otherwise, `blocking`
returns whatever `fn` returns.

If `fn` throws an exception, the exception propagates to the calling
microthread when it resumes.

The thread pool is lazily initialized on first use. The pool size is
`max(4, hardware_concurrency())`. Pool threads spend most of their time
blocked in the kernel, so this count does not represent CPU contention.

Use `blocking` for any operation that would stall the cooperative scheduler:

- File I/O (read, write, stat)
- DNS lookups via POSIX `getaddrinfo`
- CPU-intensive computation
- Third-party library calls that perform blocking syscalls
- Any operation whose duration is unpredictable

Do **not** use `blocking` for operations that CSP already handles
asynchronously, such as socket I/O (use `csp::io`) or timers (use
`csp::sleep` / `csp::after`).

### Transition rules

```
blocking(fn) ───────────────────➤ calling MT suspended; fn submitted to pool thread;
                                  pool thread runs fn();
                                  MT rescheduled on completion;
                                  → std::invoke_result_t<Fn>

blocking(fn) ─┤fn throws e├────➤ e propagated to calling MT on resume
```

### Example

```cpp
#include "csp.h"
#include "csp/blocking.h"

#include <fstream>
#include <string>

csp::spawn([] {
    // Offload a blocking file read to the thread pool.
    std::string contents = csp::blocking([] {
        std::ifstream f("/etc/hostname");
        return std::string(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
    });
    // Back on the microthread scheduler with the result.
});
csp::schedule();
```
