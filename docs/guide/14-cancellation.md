# Cancellation

CSP provides cooperative, scope-based cancellation that cascades from parent
to child imps automatically. Unlike Go's explicit `context` parameter, CSP
uses dynamic scoping -- the cancellation state is invisible to user code
except through two functions: `cancellation()` and `done()`.

## Creating a cancellation scope

Call `cancellation()` to create a scope. It returns an RAII `cancel_guard`:

```cpp
#include "csp.h"
using namespace csp;

spawn([]{
    auto guard = cancellation();

    spawn([]{
        // This child inherits the cancel scope.
        int val;
        switch (prialt(done(), work.r >> val)) {
        case ~0:  // scope was cancelled
            return;
        case  1:
            process(val);
            break;
        }
    });

    // ... do work ...
    guard();  // cancel explicitly
});
```

## Observing cancellation

`done()` returns a `chan_op<>` that fires when the scope is cancelled.
Use it as an arm in `prialt`:

```cpp
int val;
switch (prialt(done(), input.r >> val)) {
case ~0:  // cancelled
    cleanup();
    return;
case  1:  // got a value
    process(val);
    break;
}
```

If no cancellation scope is active, `done()` returns an inactive
operation that `prialt` skips -- so code using `done()` works correctly
both inside and outside a cancellation scope.

## Explicit vs automatic cancellation

Cancel explicitly by calling the guard:

```cpp
guard();                                              // cancel with canceled{}
guard(std::make_exception_ptr(my_error("reason")));   // cancel with custom reason
```

If the guard is destroyed without explicit cancellation, the destructor
auto-cancels with `canceled{}`. This ensures child imps are always notified.

Cancellation is idempotent. Calling `guard()` multiple times is safe.

## Cancellation reason

After cancellation, retrieve the reason:

```cpp
auto reason = cancel_reason();
if (reason) {
    try {
        std::rethrow_exception(reason);
    } catch (std::runtime_error const& e) {
        log("cancelled: %s", e.what());
    }
}
```

## Cascading

Cancellation scopes cascade automatically. When a parent scope is cancelled,
all child scopes are cancelled with the parent's reason:

```cpp
spawn([]{
    auto parent = cancellation();

    spawn([]{
        auto child = cancellation();

        spawn([]{
            // Watches the child scope, which is watching the parent.
            prialt(done());
            // Fires when either child or parent is cancelled.
        });

        prialt(done());
    });

    parent();  // cascades to child and grandchild
});
```

Cancelling a child does **not** affect the parent. Cancellation flows
top-down only.

## Deadlines

Pass a duration or time_point to `cancellation()` to add a deadline. The scope
auto-cancels with `timed_out{}` when the deadline expires:

```cpp
spawn([]{
    auto guard = cancellation(std::chrono::seconds(5));

    spawn([]{
        while (true) {
            int val;
            switch (prialt(done(), work.r >> val)) {
            case ~0:  return;  // timed out (or explicitly cancelled)
            case  1:  process(val); break;
            }
        }
    });

    prialt(done());  // wait for deadline or explicit cancel
});
```

The guard can still be used to cancel early -- if `guard()` is called before
the deadline, the scope is cancelled with `canceled{}` (not `timed_out{}`).

`timed_out` inherits from `canceled`, so `catch (canceled const&)` catches
both.

## Sleep and I/O

When a cancellation scope is active, `sleep`, `sleep_until`, and the I/O
functions (`io::read`, `io::write`, `io::accept`, `io::connect`) automatically
race against the cancel signal. If cancelled during one of these operations,
they rethrow the cancel reason -- `canceled{}` for explicit cancel,
`timed_out{}` for deadline expiry:

```cpp
spawn([]{
    auto guard = cancellation(std::chrono::seconds(5));

    spawn([]{
        try {
            csp::sleep(std::chrono::seconds(60));
        } catch (csp::timed_out const&) {
            // Deadline expired.
        } catch (csp::canceled const&) {
            // Explicitly cancelled before deadline.
        }
    });

    prialt(done());
});
```

When no cancellation scope is active, sleep and I/O have zero overhead --
the cancel check is a single dynamic-variable lookup that returns null.

## Channel operations

Channel operations (`<<`, `>>`, `alt`, `prialt`) are **not** automatically
cancellation-aware. To cancel a channel wait, include `done()` as an
explicit arm:

```cpp
int val;
switch (prialt(done(), ch.r >> val)) {
case ~0:  return;   // cancelled
case  1:  break;    // got value
}
```

This is by design: there is no coherent way to implicitly cancel a channel
operation, since the caller needs to decide what to do when cancellation fires
versus when data arrives.
