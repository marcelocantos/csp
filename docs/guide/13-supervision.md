# Supervision

Real programs need to handle worker failures gracefully. CSP provides
`worker_group` -- a monitor that watches a set of named worker imps and
restarts them when they fail.

## Worker groups

A `worker_group` has two public members: a `workers` map and a `policy`.
Populate them, then call `run()`:

```cpp
#include "csp.h"
using namespace csp;

worker_group wg;
wg.workers = {
    {"reader",  reader_fn},
    {"writer",  writer_fn},
    {"logger",  logger_fn},
};
wg.policy.max_restarts = 5;
wg.policy.window = 10s;

wg.run();  // blocks until all workers exit or escalation
```

## Restart policy

When a worker throws an exception, the group restarts it according to the
policy. When a worker returns normally, it is marked done and not restarted.

```cpp
struct restart_policy {
    int max_restarts = 3;   // max failures within window before escalation
    duration window = 5s;   // sliding time window
    duration backoff = 0s;  // delay before each restart
};
```

If a worker fails more than `max_restarts` times within `window`, the group
stops and throws `max_restarts_exceeded`. This is called **escalation**.

## Building supervision trees

Because `worker_group` defines `operator()`, it is itself a callable -- so
it can be used directly as a worker in a parent group:

```cpp
worker_group inner;
inner.workers = {{"pool", db_pool}, {"migration", db_migrate}};
inner.policy.max_restarts = 3;

worker_group outer;
outer.workers["database"] = std::move(inner);
outer.workers["web"]      = web_server;
outer.run();
```

If the inner group escalates (throws `max_restarts_exceeded`), the outer
group catches the exception and applies its own restart policy. This creates
a tree where failures bubble up through layers of supervision until they are
either handled (restarted) or reach the root.

## Current limitations

The current implementation uses a **one-for-one** strategy: only the failed
worker is restarted. Other strategies (one-for-all, rest-for-one) and
graceful shutdown via killswitch channels are planned for future versions.

## Next steps

- [`docs/reference/supervisor.md`](../reference/supervisor.md) -- full API
  reference for `restart_policy`, `worker_group`, and `max_restarts_exceeded`
