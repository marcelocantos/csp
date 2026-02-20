# Dynamic Scoping

Imps communicate through channels, but sometimes you need ambient
context that flows through spawn chains without being threaded explicitly via
function parameters. `csp::dynamic<T>` provides **dynamic-scoped variables**
for imps -- the coroutine equivalent of thread-local storage, with
copy-on-write isolation.

## Basic usage

```cpp
#include "csp.h"

static csp::dynamic<int> request_id;
static csp::dynamic<std::string> user("anonymous");

csp::spawn([]{
    csp::local l{request_id = 42, user = std::string("alice")};

    printf("id=%d user=%s\n", *request_id, (*user).c_str());
    // id=42 user=alice
});
```

`*var` reads the current value. `var = val` creates a **binding** -- a deferred
mutation that only takes effect when installed in a `csp::local` scope. When
the `local` goes out of scope, the variable reverts to its previous value.

If no value has been set and the type is default-constructible, `*var` returns
the default. You can supply an explicit default: `dynamic<int> count(99)`.

## Scoped bindings with `local`

`csp::local` is the primary way to set dynamic variables. It saves the current
context, applies the binding(s), and restores the context when destroyed:

```cpp
static csp::dynamic<int> level;

csp::spawn([]{
    csp::local l1{level = 1};
    assert(*level == 1);
    {
        csp::local l2{level = 2};
        assert(*level == 2);
    }
    // Restored to 1.
    assert(*level == 1);
});
```

Multiple variables can be bound in a single `local`:

```cpp
csp::local l{depth = 1, trace_id = 42, user = std::string("alice")};
```

**Bare assignment asserts**: `level = 42;` without `csp::local` triggers a
debug assertion. The `[[nodiscard]]` attribute also produces a compile-time
warning.

## Spawn inheritance

When an imp spawns a child, the child inherits the parent's context:

```cpp
static csp::dynamic<int> depth;

csp::spawn([]{
    csp::local l{depth = 1};

    csp::spawn([]{
        // Child sees parent's value.
        assert(*depth == 1);

        csp::local l2{depth = 2};
        assert(*depth == 2);
    });

    // Parent is unaffected by child's write.
    assert(*depth == 1);
});
```

This is **copy-on-write**: the child starts with a snapshot of the parent's
context. Bindings in the child create a new path in a persistent hash array
mapped trie (HAMT) -- the parent's context is never mutated.

## Context transfer over channels

A imp's context can be captured and sent to another imp:

```cpp
static csp::dynamic<int> trace_id;

auto [w, r] = csp::chan<csp::context>{};

csp::spawn([w = std::move(w)]{
    csp::local l{trace_id = 42};
    w << csp::context::current();   // capture and send
});

csp::spawn([r = std::move(r)]{
    csp::context ctx;
    r >> ctx;

    assert(*trace_id == 0);         // default before injection

    {
        csp::context_scope scope(ctx);   // install foreign context
        assert(*trace_id == 42);         // sees sender's value
    }

    assert(*trace_id == 0);         // restored
});
```

This enables patterns like distributed tracing where a request ID or
correlation context follows a request across imp boundaries without
being passed through every function signature.

## How it works

Each `dynamic<T>` variable holds a unique `context_key`. Each imp
carries a pointer to an immutable HAMT root. Reads do an O(log32 N) lookup in
the trie. `csp::local` saves the current root, creates a new root via
path-copying, and restores the old root on destruction. Roots are
reference-counted, so they are freed when no imp references them.

Because the HAMT is persistent (structurally shared), spawn inheritance is
O(1): the child simply copies the parent's root pointer and increments its
refcount.

## Imp-local variables

`csp::imp_local<T>` is the imp analog of `thread_local` -- per-imp
storage that is **not inherited** by child imps and supports direct
(non-deferred) mutation:

```cpp
static csp::imp_local<int> call_count;

csp::spawn([]{
    call_count = *call_count + 1;  // direct write
    assert(*call_count == 1);

    csp::spawn([]{
        // Child starts fresh -- not inherited.
        assert(*call_count == 0);
    });
});
```

Use `imp_local<T>` for per-imp counters, caches, or scratch state that
shouldn't leak into child imps. Use `dynamic<T>` when you want ambient
context that flows through spawn chains.

## Summary

| Type | Purpose |
|------|---------|
| `dynamic<T>` | Dynamic-scoped variable (inherited, scoped via `local`) |
| `imp_local<T>` | Imp-local variable (not inherited, direct write) |
| `local` | RAII scoped binding: `local l{var = val}` |
| `context` | Copyable handle to an HAMT root (sendable over channels) |
| `context_scope` | Install a foreign context (from `context::current()`) |

| Operation | Semantics |
|-----------|-----------|
| `*var` | Read current value |
| `var = val` | `dynamic<T>`: create binding (pass to `local`); `imp_local<T>`: direct write |
| `local l{var = val}` | Install `dynamic<T>` binding; reverts when `l` is destroyed |
| `context::current()` | Capture the current imp's context |
| `context_scope scope(ctx)` | Save current, install foreign context |
