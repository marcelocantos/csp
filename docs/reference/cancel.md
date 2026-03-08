# Cancellation Reference

Cooperative, scope-based cancellation for imps. Cancellation cascades from
parent to child scopes automatically.

All types live in `namespace csp`. Header: `#include "csp.h"`.

---

## Table of Contents

1. [csp::canceled](#cspcanceled) -- exception type
2. [csp::timed_out](#csptimed_out) -- deadline exception type
3. [csp::cancel_guard](#cspcancel_guard) -- RAII cancellation scope
4. [csp::cancellation](#cspcancellation) -- create a cancellation scope (with optional deadline)
5. [csp::done](#cspdone) -- channel operation for observing cancellation
6. [csp::cancel_reason](#cspcancel_reason) -- retrieve the cancellation reason
7. [Cancellation-aware primitives](#cancellation-aware-primitives) -- sleep and I/O

---

## csp::canceled

Exception thrown when an imp is cancelled during a cancellation-aware operation
(sleep or I/O).

### Signature

```cpp
struct canceled : csp::error {
    canceled();  // what() == "canceled"
};
```

---

## csp::timed_out

Exception thrown when a deadline expires. Inherits from `canceled`, so code
catching `canceled` also catches timeouts.

### Signature

```cpp
struct timed_out : canceled {
    timed_out();  // what() == "timed out"
};
```

---

## csp::cancel_guard

RAII guard that owns a cancellation scope. When destroyed, auto-cancels with
`canceled{}` if not already cancelled. Move-only.

### Signature

```cpp
class cancel_guard {
public:
    cancel_guard(cancel_guard&&) noexcept;
    cancel_guard& operator=(cancel_guard&&) noexcept;
    ~cancel_guard();  // auto-cancels if not already cancelled

    void operator()();                    // cancel with canceled{}
    void operator()(std::exception_ptr);  // cancel with specific reason
};
```

### Description

Calling `guard()` cancels the scope with a default `canceled{}` exception.
Calling `guard(ep)` cancels with a specific reason. Both are idempotent --
the second and subsequent calls are no-ops.

The destructor auto-cancels with `canceled{}` if the scope has not been
cancelled explicitly. This ensures all child imps are notified even if the
guard goes out of scope without explicit cancellation.

---

## csp::cancellation

Create a new cancellation scope. Returns a `cancel_guard` that owns the scope.

### Signature

```cpp
cancel_guard cancellation();
cancel_guard cancellation(duration d);
cancel_guard cancellation(time_point tp);
```

### Description

`cancellation()` creates a new cancel state and binds it to the current imp's
dynamic scope. Child imps spawned within this scope inherit the binding and
can observe cancellation via `done()`.

If a parent cancellation scope exists (from an enclosing `cancellation()` call
in the spawn chain), a cascade imp is automatically spawned. When the parent
is cancelled, the cascade imp propagates the parent's reason to the child
scope.

The `duration` and `time_point` overloads add a deadline: a timer imp is
spawned that cancels the scope with `timed_out{}` when the deadline expires.
The guard can still be used to cancel early with a different reason.

### Transition rules ([syntax](transition-rules.md))

```
cancellation()                           ➤ new cancel_state; dynamic scope
                                           binding installed; → cancel_guard

cancellation(d) / cancellation(tp)       ➤ same as above, plus timer imp
                                           spawned; fires timed_out{} at deadline

cancellation() ─┤parent scope exists├─── ➤ cascade imp spawned: watches parent
                                           signal, propagates cancel to child

guard()                                  ➤ cancel_state.cancelled = true;
                                           trigger writer dropped;
                                           all vultures on signal fire

guard(ep)                                ➤ cancel_state.cancelled = true;
                                           cancel_state.reason = ep;
                                           trigger writer dropped

~guard ─┤not yet cancelled├──────────── ➤ cancel with canceled{}

~guard ─┤already cancelled├──────────── ➤ no-op (binding restored)
```

---

## csp::done

Return a channel operation suitable for `alt`/`prialt` that fires when the
current cancellation scope is cancelled.

### Signature

```cpp
chan_op<> done();
```

### Description

Returns a vulture (`~reader`) on the internal signal channel. When the scope
is cancelled, the trigger writer is dropped, causing the vulture to fire.

If no cancellation scope is active (no enclosing `cancellation()` in the
spawn chain), returns a default-constructed `chan_op<>` which is skipped by
`alt`/`prialt`.

### Example

```cpp
auto guard = csp::cancellation();

csp::spawn([]{
    int val;
    switch (csp::prialt(csp::done(), ch.r >> val)) {
    case ~0:  // cancelled
        break;
    case  1:  // received val
        process(val);
        break;
    }
});
```

---

## csp::cancel_reason

Retrieve the exception that caused cancellation.

### Signature

```cpp
std::exception_ptr cancel_reason();
```

### Description

Returns the `exception_ptr` stored when the scope was cancelled. Returns
`nullptr` if no cancellation scope is active or if the scope has not been
cancelled.

---

## Cancellation-aware primitives

When a cancellation scope is active, the following primitives automatically
race against the cancel signal:

| Primitive | Behavior on cancel |
|---|---|
| `csp::sleep(d)` / `csp::sleep_until(tp)` | Rethrows cancel reason (`canceled{}` or `timed_out{}`) |
| `csp::io::read(fd, ...)` | Rethrows cancel reason |
| `csp::io::write(fd, ...)` | Rethrows cancel reason |
| `csp::io::accept(fd, ...)` | Rethrows cancel reason |
| `csp::io::connect(fd, ...)` | Rethrows cancel reason |

When no cancellation scope is active, these primitives behave exactly as before
with zero overhead.

Channel operations (`<<`, `>>`, `alt`, `prialt`) are **not** automatically
cancellation-aware. Use `done()` as an explicit arm in `prialt` to observe
cancellation alongside channel operations.

### Implementation note

Cancellation-aware sleep and I/O spawn a helper imp that performs the raw
operation, then race the helper's completion against the cancel signal via
`prialt`. When cancellation fires, the helper imp remains blocked until the
underlying operation completes (or the fd is closed), then exits cleanly.
