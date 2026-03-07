# Supervision Reference

> **Deprecated**: Prefer [`on_exit`](imp-exit.md) with `supervised()` for new
> code. `worker_group` is retained for backward compatibility but offers less
> flexibility than the composable `on_exit` + `supervised` pattern.

Worker lifecycle management with automatic restart and failure escalation.

All types live in `namespace csp`. Header: `#include "csp/supervisor.h"`.

---

## Table of Contents

1. [restart\_policy](#restart_policy) -- restart limits and backoff
2. [worker\_group](#worker_group) -- monitored set of worker imps
3. [max\_restarts\_exceeded](#max_restarts_exceeded) -- escalation exception

---

## restart\_policy

```cpp
struct restart_policy {
    int max_restarts = 3;
    csp::duration window = std::chrono::seconds(5);
    csp::duration backoff = csp::duration::zero();
};
```

| Field | Default | Description |
|---|---|---|
| `max_restarts` | `3` | Maximum restarts allowed within `window` before escalation. |
| `window` | `5s` | Sliding time window for counting restarts. |
| `backoff` | `0s` | Delay before each restart (constant). |

A restart counter is maintained per worker. Before each restart, timestamps
older than `now - window` are pruned. If the remaining count reaches
`max_restarts`, the group throws `max_restarts_exceeded` instead of
restarting.

---

## worker\_group

A monitor that owns a set of named worker imps. When a worker throws, the
group restarts it according to the configured `restart_policy`. When a worker
returns normally, it is marked done and not restarted.

```cpp
class worker_group {
public:
    std::unordered_map<std::string, std::function<void()>> workers;
    restart_policy policy;

    void operator()();
    void run();
};
```

### Setup

Populate the `workers` map and optionally configure `policy` before calling
`run()`. Do not modify these members while `run()` is executing.

```cpp
worker_group wg;
wg.workers = {
    {"reader", reader_fn},
    {"writer", writer_fn},
};
wg.policy.max_restarts = 5;
wg.run();
```

### Running

`operator()()` spawns all workers and enters a monitoring loop. It blocks
until all workers have exited normally or escalation occurs. `run()` is a
named convenience that calls `operator()()`.

Because `worker_group` defines `operator()`, it is itself a callable and
converts directly to `std::function<void()>`. This makes nesting trivial:

```cpp
worker_group inner;
inner.workers = {{"a", fa}, {"b", fb}};

worker_group outer;
outer.workers["inner"] = std::move(inner);
outer.run();
```

### Restart semantics

- **Normal exit** (worker returns): the worker is marked done and not
  restarted.
- **Exception exit** (worker throws): the sliding window is checked. If the
  restart count within the window is below `max_restarts`, the worker is
  restarted (after an optional `backoff` delay). Otherwise, the group throws
  `max_restarts_exceeded`.

The current strategy is **one-for-one**: only the failed worker is restarted.
Other workers are unaffected.

### Escalation

When `max_restarts` is exceeded, `run()` throws `max_restarts_exceeded`. Any
remaining workers become detached (their join handles are dropped). Detached
workers' exceptions are routed to `global_exception_handler`.

If the group is itself a worker in a parent group, the parent sees the
exception and applies its own restart policy, forming a supervision tree.

---

## max\_restarts\_exceeded

```cpp
struct max_restarts_exceeded : csp::error {
    std::string worker_name;
    std::exception_ptr cause;

    max_restarts_exceeded(std::string name, std::exception_ptr ex);
};
```

Thrown by `worker_group::run()` when a worker exceeds the restart limit.
`worker_name` identifies the failed worker; `cause` holds the exception
from the final failure.

---

## Example: supervision tree

```cpp
#include "csp.h"

using namespace csp;

int main() {
    init_runtime();

    spawn([]() {
        worker_group root;
        root.policy.max_restarts = 5;

        worker_group db;
        db.policy.max_restarts = 3;
        db.workers = {{"pool", db_pool}, {"migration", db_migrate}};

        worker_group web;
        web.policy.max_restarts = 10;
        web.workers = {{"accept", accept_loop}, {"handlers", handler_pool}};

        root.workers = {{"db", std::move(db)}, {"web", std::move(web)}};
        root.run();
    });

    schedule();
}
```

Each nested `worker_group` is a callable that blocks in its monitoring loop.
If an inner group escalates, the outer group sees the exception and applies
its own policy.
