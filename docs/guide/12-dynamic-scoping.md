# Dynamic Scoping

Microthreads communicate through channels, but sometimes you need ambient
context that flows through spawn chains without being threaded explicitly via
function parameters. `csp::dynamic<T>` provides **dynamic-scoped variables**
for microthreads -- the coroutine equivalent of thread-local storage, with
copy-on-write isolation.

## Basic usage

```cpp
#include <csp/dynamic.h>

static csp::dynamic<int> request_id;
static csp::dynamic<std::string> user("anonymous");

csp::spawn([]{
    request_id = 42;
    user = std::string("alice");

    printf("id=%d user=%s\n", *request_id, (*user).c_str());
    // id=42 user=alice
});
```

`*var` reads the current value. `var = val` writes a new value. If no value
has been set and the type is default-constructible, `*var` returns the default.
You can also supply an explicit default: `dynamic<int> count(99)`.

## Spawn inheritance

When a microthread spawns a child, the child inherits the parent's context:

```cpp
static csp::dynamic<int> depth;

csp::spawn([]{
    depth = 1;

    csp::spawn([]{
        // Child sees parent's value.
        assert(*depth == 1);

        depth = 2;
        assert(*depth == 2);
    });

    // Parent is unaffected by child's write.
    assert(*depth == 1);
});
```

This is **copy-on-write**: the child starts with a snapshot of the parent's
context. Writes in the child create a new path in a persistent hash array
mapped trie (HAMT) -- the parent's context is never mutated.

## context_scope

`context_scope` saves the current context on construction and restores it on
destruction, providing RAII-scoped modifications:

```cpp
static csp::dynamic<int> level;

csp::spawn([]{
    level = 1;
    {
        csp::context_scope scope;
        level = 2;
        assert(*level == 2);
    }
    // Restored to 1.
    assert(*level == 1);
});
```

Scopes nest naturally:

```cpp
level = 1;
{
    csp::context_scope outer;
    level = 2;
    {
        csp::context_scope inner;
        level = 3;
        assert(*level == 3);
    }
    assert(*level == 2);
}
assert(*level == 1);
```

## Context transfer over channels

A microthread's context can be captured and sent to another microthread:

```cpp
static csp::dynamic<int> trace_id;

auto [w, r] = csp::chan<csp::context>{};

csp::spawn([w = std::move(w)]{
    trace_id = 42;
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
correlation context follows a request across microthread boundaries without
being passed through every function signature.

## How it works

Each `dynamic<T>` variable holds a unique `context_key`. Each microthread
carries a pointer to an immutable HAMT root. Reads do an O(log32 N) lookup in
the trie. Writes create a new root via path-copying (the old root is
unchanged). Roots are reference-counted, so they are freed when no microthread
references them.

Because the HAMT is persistent (structurally shared), spawn inheritance is
O(1): the child simply copies the parent's root pointer and increments its
refcount.

## Summary

| Type | Purpose |
|------|---------|
| `dynamic<T>` | Typed dynamic-scoped variable |
| `context_key` | Unique identifier (internal to `dynamic`) |
| `context` | Copyable handle to an HAMT root (sendable over channels) |
| `context_scope` | RAII save/restore of the current context |

| Operation | Semantics |
|-----------|-----------|
| `*var` | Read current value (O(log32 N)) |
| `var = val` | Write new value (path-copy, O(log32 N)) |
| `context::current()` | Capture the current microthread's context |
| `context_scope scope` | Save current context; restore on destruction |
| `context_scope scope(ctx)` | Save current, install foreign context |
