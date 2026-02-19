# Signals Reference

Receive Unix signals as values on a CSP channel.

All types live in `namespace csp::signal`. Header: `#include "csp/signal.h"`.

---

## Table of Contents

1. [signal::notify](#signalnotify) -- subscribe to Unix signals

---

## signal::notify

Return a reader that emits signal numbers whenever specified Unix signals are
delivered to the process.

### Signature

```cpp
namespace csp::signal {

reader<int> notify(std::initializer_list<int> sigs);

}
```

**Header:** `#include "csp/signal.h"`

### Description

`notify` installs signal handlers for each signal number in `sigs` and returns
a `reader<int>` that produces the signal number each time one of those signals
is delivered to the process.

Internally, `notify` uses the self-pipe trick: a pipe is created and the signal
handler writes the signal number (as a byte) to the pipe's write end. A
microthread reads from the pipe via `io::read` (non-blocking, reactor-driven)
and forwards each signal number to the returned reader through a channel.
Because the handler only performs atomic loads and `write()` -- both
async-signal-safe -- the design is safe even when signals arrive during channel
operations.

**Reactor required.** `notify` uses `io::read` internally, so the I/O reactor
must be running. Call `init_runtime()` before using signal channels, or ensure
`schedule()` drives the reactor in single-threaded mode.

**Handler installation.** Signal handlers are installed via `sigaction` with
`SA_RESTART`. Installation is idempotent per signal number -- multiple `notify`
calls for the same signal share the same handler. Each call creates its own
pipe, so multiple readers for the same signal each receive every delivery
independently.

**Cleanup.** Dropping the returned reader triggers a sentinel microthread that
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

### Transition rules

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
#include "csp/signal.h"
#include "csp/timer.h"

#include <csignal>

int main() {
    csp::init_runtime();

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
#include "csp/signal.h"

#include <csignal>
#include <cstdio>

int main() {
    csp::init_runtime();

    csp::spawn([] {
        auto sig = csp::signal::notify({SIGINT, SIGTERM});

        int signo = sig.read();
        std::printf("caught signal %d, shutting down\n", signo);
    });
    csp::schedule();
    csp::shutdown_runtime();
}
```
