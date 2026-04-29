# log_aggregator — Multi-Source Log Aggregator

Demonstrates multi-source log aggregation with per-severity routing,
time-window counting, and threshold alerting using CSP channels and timers.

## How to Run

```bash
make build/normal/examples/log_aggregator
./build/normal/examples/log_aggregator
```

Or build all examples:

```bash
make examples
```

## Expected Output

```
Log Aggregator — 4 services, 1-second windows, alert at >=3 errors

  [DEBUG] api  request received
  [INFO ] auth session created: user=42
  [INFO ] web  GET /index.html 200
  ...
  [ERROR] auth invalid signature — rejecting
  [ERROR] auth replay attack detected
  [ERROR] auth token expired: forced logout

  *** ALERT: 3 errors in current window (threshold=3) ***

  ...
  -- Window 1 summary --
    api   info=5  warn=2
    auth  info=4  warn=1
    db    info=4  warn=1
    web   info=5  warn=2

  Aggregator done.
```

## Architecture

```
web-gen ─┐
api-gen ─┤                          ┌─► info_ch ─► aggregator
db-gen  ─┼─► merge ─► router-imp ──┤
auth-gen─┘                          ├─► warn_ch ─► aggregator
                                    └─► error_ch ─► alert-monitor
```

Four generator imps simulate log tailing from `web`, `api`, `db`, and `auth`
services, emitting pre-canned log entries at staggered intervals. The `auth`
service injects a burst of errors mid-run to trigger the alert threshold.

**Merge** (`csp::part::merge`) fan-ins all four sources into a single stream.

**Router imp** reads the merged stream, prints each entry, and fans out to
per-severity channels (`info_ch`, `warn_ch`, `error_ch`). Debug entries are
printed but not routed further.

**Alert monitor imp** counts errors per 1-second window via `csp::tick(1s)`.
Prints an alert banner when the count reaches the threshold (3). Resets the
counter each tick.

**Aggregator imp** counts `INFO`/`WARN` events per service per 1-second window,
printing a summary table on each tick.

## CSP Features Demonstrated

| Feature | Where used |
|---|---|
| `csp::part::merge` | Fan-in from four generator channels |
| `csp::tick` | Periodic 1-second window boundary |
| `csp::alt` | Select over multiple channels simultaneously |
| Severity fan-out | Router imp dispatches to typed severity channels |
| Channel death propagation | Alert monitor exits when `error_ch` writer dies |
| `csp::schedule()` | Top-level scheduler entry point |

## Trade-offs

Log streams are **channel-based**, not real file tails. Generator imps write
directly to `chan<LogEntry>` channels rather than appending to disk files and
tailing them with `inotify`/`kqueue`. This keeps the example self-contained and
focuses on the aggregation patterns (merge → parse → route → window → alert)
rather than I/O mechanics. Real file tailing would substitute `csp::part::io::lines(fd)`
for the generator imp, with identical downstream topology.
