# Signals Reference

Receive Unix signals as values on a CSP channel.

All types live in `namespace csp::signal`. Header: `#include "csp.h"`.

---

## Table of Contents

1. [csp::signal::notify](#cspsignalnotify) -- subscribe to Unix signals

---

## csp::signal::notify

Return a reader that emits signal numbers whenever specified Unix signals are
delivered to the process.

### Signature

```cpp
namespace csp::signal {

reader<int> notify(std::initializer_list<int> sigs);

}
```

**Header:** `#include "csp.h"`

### Description

`notify` installs signal handlers for each signal number in `sigs` and returns
a `reader<int>` that produces the signal number each time one of those signals
is delivered to the process.

Internally, `notify` uses the self-pipe trick: a pipe is created and the signal
handler writes the signal number (as a byte) to the pipe's write end. A
imp reads from the pipe via `io::read` (non-blocking, reactor-driven)
and forwards each signal number to the returned reader through a channel.
Because the handler only performs atomic loads and `write()` -- both
async-signal-safe -- the design is safe even when signals arrive during channel
operations.

**Reactor required.** `notify` uses `io::read` internally, so the I/O reactor
must be running. The runtime auto-initializes on first use with M:N threading
enabled by default, which starts the reactor.

**Handler installation.** Signal handlers are installed via `sigaction` with
`SA_RESTART`. Installation is idempotent per signal number -- multiple `notify`
calls for the same signal share the same handler. Each call creates its own
pipe, so multiple readers for the same signal each receive every delivery
independently.

**Cleanup.** Dropping the returned reader triggers a sentinel imp that
closes the pipe write end, causing the producer's `io::read` loop to see EOF
and exit. The signal handler remains installed but becomes a harmless no-op
once the pipe's signal mask is cleared.

**Multiple signals.** A single `notify` call can subscribe to several signals
at once. Each delivery produces the specific signal number that fired, so the
caller can distinguish between them:

```cpp
auto sig = csp::signal::notify({SIGINT, SIGTERM});
int s = sig.read();  // s == SIGINT or s == SIGTERM
```

**Alt/prialt integration.** The returned reader is an ordinary `reader<int>`
and works in any context where readers are used: range-for loops,
`alt`/`prialt` arms, `stream_to`, combinators, etc.

### Transition rules ([syntax](transition-rules.md))

```
notify(sigs)                ──────────────────➤ pipe created; handlers installed;
                                                producer MT spawned; sentinel MT spawned;
                                                → reader<int>

signal delivered ─┤reader alive├──────────────➤ signal number written to pipe;
                                                producer reads pipe → writes to channel;
                                                reader receives int

signal delivered ─┤reader dropped├────────────➤ write to pipe (SIGPIPE suppressed);
                                                byte discarded (no consumer)

reader dropped   ──────────────────────────────➤ sentinel detects ~reader; clears mask;
                                                 closes pipe write end;
                                                 producer sees EOF → exits;
                                                 pipe read end closed
```

### Example

Graceful shutdown on SIGINT or SIGTERM:

```cpp
#include "csp.h"

#include <csignal>

int main() {
    csp::spawn([] {
        auto sig = csp::signal::notify({SIGINT, SIGTERM});
        auto [w, r] = csp::chan<int>{};

        // Worker: produces values until told to stop.
        csp::spawn([w = std::move(w)] {
            for (int i = 0; ; ++i) {
                if (!(w << i)) return;
                csp::sleep(std::chrono::milliseconds(100));
            }
        });

        // Main loop: process values or shut down on signal.
        bool running = true;
        while (running) {
            int val, signo;
            switch (csp::prialt(r >> val, sig >> signo)) {
            case 0: /* process val */ break;
            case 1: running = false; break;   // signal received
            }
        }
        // Reader `r` dropped here; worker's next write fails, worker exits.
        // Reader `sig` dropped here; sentinel cleans up signal pipe.
    });
    csp::schedule();
    csp::shutdown_runtime();
}
```

A simpler pattern that just waits for a signal:

```cpp
#include "csp.h"

#include <csignal>
#include <cstdio>

int main() {
    csp::spawn([] {
        auto sig = csp::signal::notify({SIGINT, SIGTERM});

        int signo = sig.read();
        std::printf("caught signal %d, shutting down\n", signo);
    });
    csp::schedule();
    csp::shutdown_runtime();
}
```

---

## Windows Console Signals

On Windows, POSIX signals are not available. CSP provides an equivalent API in
`namespace csp::win::signal` that converts Windows console control events into
channel reads.

**Header:** `#include "csp.h"`

### csp::win::signal::notify

```cpp
namespace csp::win::signal {

reader<DWORD> notify(std::initializer_list<DWORD> events);

}
```

Returns a reader that emits the console control event type (as `DWORD`) each
time one of the specified events is delivered. Installs a handler via
`SetConsoleCtrlHandler` (once, idempotent). Requires the M:N runtime (auto-initialized by default).

**Supported events:**

| Constant | Value | Trigger |
|---|---|---|
| `CTRL_C_EVENT` | 0 | Ctrl-C |
| `CTRL_BREAK_EVENT` | 1 | Ctrl-Break |
| `CTRL_CLOSE_EVENT` | 2 | Console window closed |
| `CTRL_LOGOFF_EVENT` | 5 | User logs off |
| `CTRL_SHUTDOWN_EVENT` | 6 | System shutdown |

The handler returns `TRUE` for registered events (suppressing the default
handler, analogous to Unix `sigaction` replacing the default). For
`CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT`, and `CTRL_SHUTDOWN_EVENT`, the process
receives a limited time window (~5 seconds) to clean up before being
terminated.

### Example

```cpp
#include "csp.h"

int main() {
    csp::spawn([] {
        auto sig = csp::win::signal::notify({CTRL_C_EVENT, CTRL_CLOSE_EVENT});

        DWORD ev = sig.read();
        // ev == CTRL_C_EVENT or CTRL_CLOSE_EVENT
        // ... clean up and exit ...
    });
    csp::schedule();
    csp::shutdown_runtime();
}
```

### Implementation

Uses a socket-pair trick (analogous to the Unix self-pipe trick). A loopback
TCP socket pair is created for each `notify()` call. The console handler sends
the event type as a byte to matching sockets. A producer imp reads via
`io::read()` and forwards events to the channel. Cleanup follows the same
sentinel pattern as Unix.
