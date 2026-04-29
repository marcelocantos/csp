# Dynamic Scoping Reference

Dynamic-scoped variables provide implicit context that flows through the
imp call and spawn hierarchy. Unlike lexical variables, dynamic
variables are visible to all callees and child imps without explicit
parameter passing. All types live in `namespace csp`. Header:
`#include "csp.h"`.

---

## Table of Contents

1. [csp::context\_key](#cspcontext_key) -- unique variable identity
2. [csp::dynamic\<T\>](#cspdynamict) -- inherited dynamic variable
3. [csp::dynamic\_binding](#cspdynamic_binding) -- deferred binding token
4. [csp::local](#csplocal) -- scoped binding installer
5. [csp::context](#cspcontext) -- portable context handle
6. [csp::context\_scope](#cspcontext_scope) -- foreign context installer
7. [csp::imp\_local\<T\>](#cspimp_localt) -- imp-local variable

---

## Overview

Dynamic variables form a layered binding environment carried by each
imp. The environment is stored as a persistent hash array mapped trie
(HAMT) rooted at `Imp::dyn_ctx_`. Each `local` scope creates a new
HAMT root via path-copy, leaving the parent root untouched. When an imp
spawns a child, the child inherits the parent's current HAMT root, giving it
a snapshot of all bindings at the point of spawn.

<!-- csp-state
state "HAMT root A" as A
state "HAMT root B\n(path-copy of A)" as B
state "HAMT root C\n(path-copy of B)" as C

[*] -> A : dynamic&lt;T&gt; default
A -> B : local l{var = val}
B -> C : spawn (child inherits B)
B -> A : ~local (restore)
C -> C : local in child (path-copy)
-->
![HAMT scoping](diagrams/hamt-scoping.svg)

---

## csp::context\_key

Unique, monotonically increasing identity for a dynamic variable.

### Signature

```cpp
class context_key {
public:
    context_key();                                  // assigns unique uint64_t
    context_key(const context_key&) = default;
    context_key& operator=(const context_key&) = default;

    uint64_t id() const;
    bool operator==(const context_key&) const;
    bool operator!=(const context_key&) const;
};
```

### Description

Each default-constructed `context_key` receives a globally unique ID from a
monotonic atomic counter. Copies compare and hash equal to the original.
`dynamic<T>` and `imp_local<T>` each hold one `context_key` as their identity
in the HAMT or per-imp map.

Users do not construct `context_key` directly; it is an implementation detail
exposed for completeness.

---

## csp::dynamic\<T\>

A typed dynamic-scoped variable. The value is inherited by child imps
via `spawn` and scoped via `local`.

### Signature

```cpp
template <typename T>
class dynamic {
public:
    dynamic();                          // default T{} if default-constructible
    explicit dynamic(T def);            // explicit default value

    T operator*() const;                // read current binding

    [[nodiscard]] dynamic_binding operator=(T val);  // create deferred binding

    dynamic(const dynamic&) = delete;
    dynamic& operator=(const dynamic&) = delete;
};
```

### Description

**Construction.** A `dynamic<T>` should be declared at namespace or `static`
local scope -- it holds a unique `context_key` and an optional default value.
If `T` is default-constructible, the parameterless constructor stores `T{}` as
the default. Otherwise, an explicit default must be provided.

**Reading.** `operator*()` performs an HAMT lookup on the current imp's
`dyn_ctx_` using the variable's key. If a binding exists, the value is returned
by value via `std::any_cast<T>`. If no binding exists, the default is returned.
It is undefined behaviour to dereference a `dynamic<T>` that has no binding and
no default (asserts in debug builds).

**Binding.** `operator=(T val)` does **not** mutate the variable. It returns a
`dynamic_binding` token that must be consumed by a `local` scope. The
`[[nodiscard]]` attribute produces a compiler warning if the return value is
discarded. A bare `var = val;` statement (without `local`) triggers an
assertion in `dynamic_binding`'s destructor.

**Non-copyable.** Each `dynamic<T>` instance owns a unique key. Copying would
create two variables sharing the same key, which is nonsensical.

### Transition rules ([syntax](transition-rules.md))

```
operator*()   ─┤binding exists├───➤ any_cast<T>(hamt_get(dyn_ctx_, key)); T
operator*()   ─┤no binding├───────➤ assert(default_); *default_
operator=(v)  ─────────────────────➤ dynamic_binding{key, any(move(v))}
```

### Example

```cpp
#include "csp.h"

static csp::dynamic<int> depth;
static csp::dynamic<std::string> name("anonymous");

csp::spawn([] {
    // Read defaults.
    assert(*depth == 0);
    assert(*name == "anonymous");

    // Bind in a local scope.
    csp::local l{depth = 1, name = std::string("root")};
    assert(*depth == 1);

    {
        csp::local l2{depth = *depth + 1};
        assert(*depth == 2);    // inner binding
        assert(*name == "root"); // inherited, not rebound
    }

    assert(*depth == 1);        // restored
});
csp::schedule();
```

---

## csp::dynamic\_binding

An opaque, move-only token representing a deferred binding of a dynamic
variable to a value. Created by `dynamic<T>::operator=`, consumed by `local`.

### Signature

```cpp
class dynamic_binding {
    // Only constructible by dynamic<T>, only consumable by local.
    friend class local;
    template <typename> friend class dynamic;

public:
    dynamic_binding(const dynamic_binding&) = delete;
    dynamic_binding& operator=(const dynamic_binding&) = delete;
    dynamic_binding& operator=(dynamic_binding&&) = delete;

    ~dynamic_binding();   // asserts if not consumed
};
```

### Description

`dynamic_binding` is maximally private. Its constructor is private, accessible
only to `dynamic<T>`. Its move constructor is private, accessible only to
`local` (which consumes the binding via fold-expression). The destructor
asserts `consumed_`, catching the common mistake of writing `var = val;` as a
bare statement instead of wrapping it in `local`.

The `[[nodiscard]]` attribute on `dynamic<T>::operator=` provides a
compile-time warning before the runtime assertion fires.

### Transition rules ([syntax](transition-rules.md))

```
dynamic<T>::operator=(v) ────────➤ dynamic_binding{key, any(v)}; consumed_ = false
local(move(binding))     ────────➤ consumed_ = true; apply to HAMT
~dynamic_binding()       ─┤consumed├────➤ (no-op)
~dynamic_binding()       ─┤!consumed├───➤ assertion failure
```

---

## csp::local

RAII scope that installs one or more dynamic variable bindings. Saves the
current HAMT root on construction and restores it on destruction.

### Signature

```cpp
class local {
public:
    template <typename... Bs>                       // Bs = dynamic_binding&&...
    local(Bs&&... bindings);                        // SFINAE: all must be dynamic_binding

    ~local();                                       // restore saved HAMT root

    local(const local&) = delete;
    local& operator=(const local&) = delete;
};
```

### States

<!-- csp-state
[*] -> active : local l{var = val}
active -> [*] : ~local (restore root)
-->
![local states](diagrams/local-states.svg)

| State  | Meaning |
|--------|---------|
| active | Bindings are in effect. The previous HAMT root is saved. |

### Description

**Construction.** The constructor saves the current `dyn_ctx_` (retaining the
HAMT root), then applies each binding in order. Each binding performs an
`hamt_assoc` path-copy, creating a new HAMT root with the key bound to the
new value. The old root is released after each step.

**Destruction.** The destructor replaces the current `dyn_ctx_` with the saved
root and releases the current root. This restores all dynamic variables to
their values before the `local` was constructed.

**Variadic.** Multiple bindings can be installed in a single `local`:

```cpp
csp::local l{x = 1, y = 2, z = std::string("hello")};
```

The SFINAE constraint ensures that only `dynamic_binding` rvalues are accepted.
Passing anything else is a compile error.

### Transition rules ([syntax](transition-rules.md))

```
local(b1, b2, ...)  ────────➤ save dyn_ctx_; hamt_assoc(b1); hamt_assoc(b2); ...
~local()            ────────➤ release current dyn_ctx_; restore saved root
```

### Example

```cpp
#include "csp.h"

static csp::dynamic<int> val;

csp::spawn([] {
    csp::local l{val = 10};
    assert(*val == 10);

    csp::spawn([&] {
        // Child inherits parent's context at spawn time.
        assert(*val == 10);

        csp::local l2{val = 99};
        assert(*val == 99);
    });

    // Parent is unaffected by child's local.
    assert(*val == 10);
});
csp::schedule();
```

---

## csp::context

A copyable, sendable handle to a snapshot of an HAMT root. Allows dynamic
variable bindings to be transferred across imp boundaries.

### Signature

```cpp
class context {
public:
    context();                                      // empty (null root)
    context(const context&);                        // retain root
    context(context&&) noexcept;                    // transfer ownership
    ~context();                                     // release root

    context& operator=(context) noexcept;           // copy-and-swap

    uintptr_t root() const;                         // raw root (internal)

    static context current();                       // snapshot current MT's context
};
```

### Description

`context` wraps a refcounted HAMT root pointer. Copying a `context` retains
the root; destruction releases it. The HAMT is persistent, so a snapshot
remains valid regardless of subsequent mutations in the original imp.

**`current()`** captures the calling imp's `dyn_ctx_` as a `context`.
This is the primary way to obtain a `context` for cross-imp transfer.

**Sendable.** `context` is copyable and can be sent over a `chan<context>`. The
receiving imp can install it via `context_scope` to temporarily adopt
the sender's dynamic variable bindings.

### Transition rules ([syntax](transition-rules.md))

```
context::current()     ────────➤ retain(g_imp->dyn_ctx_); context{root}
context(copy)          ────────➤ retain(copy.root_)
context(move)          ────────➤ transfer root_; source.root_ = 0
~context()             ─┤root != 0├───➤ release(root_)
~context()             ─┤root == 0├───➤ (no-op)
```

### Example

```cpp
#include "csp.h"

static csp::dynamic<int> val;

csp::spawn([] {
    auto ch = csp::chan<csp::context>();

    csp::spawn([&] {
        csp::local l{val = 42};
        ch.w << csp::context::current();
    });

    csp::context ctx;
    ch.r >> ctx;

    assert(*val == 0);               // default, no binding here
    {
        csp::context_scope scope(ctx);
        assert(*val == 42);          // sender's binding is active
    }
    assert(*val == 0);               // restored
});
csp::schedule();
```

---

## csp::context\_scope

RAII guard that saves the current imp's dynamic context and installs
a foreign context obtained from another imp.

### Signature

```cpp
class context_scope {
public:
    explicit context_scope(const context& ctx);     // save + install
    ~context_scope();                               // restore

    context_scope(const context_scope&) = delete;
    context_scope& operator=(const context_scope&) = delete;
};
```

### States

<!-- csp-state
[*] -> installed : context_scope(ctx)
installed -> [*] : ~context_scope (restore)
-->
![context_scope states](diagrams/context-scope-states.svg)

| State     | Meaning |
|-----------|---------|
| installed | Foreign context is active. The previous `dyn_ctx_` is saved. |

### Description

**Construction.** Saves the current `dyn_ctx_` (with retain), then replaces it
with the provided context's root (with retain). The old root is released.

**Destruction.** Restores the saved `dyn_ctx_` and releases the installed
foreign root.

Use `context_scope` when an imp receives a `context` over a channel and
needs to temporarily operate under the sender's dynamic variable bindings. For
binding variables directly, prefer `local`.

### Transition rules ([syntax](transition-rules.md))

```
context_scope(ctx)  ────────➤ save dyn_ctx_; retain(ctx.root); install ctx.root; release(old)
~context_scope()    ────────➤ release(current dyn_ctx_); restore saved root
```

### Example

```cpp
#include "csp.h"

static csp::dynamic<std::string> request_id("none");

csp::spawn([] {
    auto ch = csp::chan<csp::context>();

    // Producer binds request_id and sends its context.
    csp::spawn([&] {
        csp::local l{request_id = std::string("req-42")};
        ch.w << csp::context::current();
    });

    // Consumer installs the producer's context.
    csp::spawn([&] {
        csp::context ctx;
        ch.r >> ctx;

        csp::context_scope scope(ctx);
        assert(*request_id == "req-42");
    });
});
csp::schedule();
```

---

## csp::imp\_local\<T\>

A imp-local variable. Each imp has its own independent value,
stored in a lazily allocated per-imp map. Unlike `dynamic<T>`,
`imp_local<T>` is **not** inherited by child imps and supports direct
assignment without `local`.

### Signature

```cpp
template <typename T>
class imp_local {
public:
    imp_local();                         // default T{} if default-constructible
    explicit imp_local(T def);           // explicit default value

    T operator*() const;                // read current value

    imp_local& operator=(T val);         // direct write

    imp_local(const imp_local&) = delete;
    imp_local& operator=(const imp_local&) = delete;
};
```

### Description

**Construction.** Like `dynamic<T>`, an `imp_local<T>` should be declared at
namespace or `static` local scope. If `T` is default-constructible, the
parameterless constructor stores `T{}` as the default.

**Reading.** `operator*()` looks up the variable's key in the current
imp's `local_ctx_` map. If found, the value is returned via
`std::any_cast<T>`. If not found, the default is returned. It is undefined
behaviour to dereference an `imp_local<T>` with no stored value and no default.

**Writing.** `operator=(T val)` stores the value directly in the current
imp's `local_ctx_` map (lazily allocated on first write). No `local`
scope is needed. The write is visible only to the current imp.

**Not inherited.** When an imp spawns a child, the child starts with an
empty `local_ctx_`. The child sees only default values for all `imp_local`
variables, regardless of the parent's state. This is the key distinction from
`dynamic<T>`.

**Storage.** Each imp's `local_ctx_` is an
`unordered_map<uint64_t, std::any>`, allocated on first write and destroyed
with the imp.

### Transition rules ([syntax](transition-rules.md))

```
operator*()   ─┤key in local_ctx_├───➤ any_cast<T>(local_ctx_[key]); T
operator*()   ─┤key absent├──────────➤ assert(default_); *default_
operator=(v)  ────────────────────────➤ local_ctx_[key] = any(move(v)); *this
```

### Example

```cpp
#include "csp.h"

static csp::imp_local<int> call_count;

csp::spawn([] {
    assert(*call_count == 0);
    call_count = *call_count + 1;
    assert(*call_count == 1);

    auto ch = csp::chan<int>();

    csp::spawn([&] {
        // Child does NOT inherit parent's call_count.
        assert(*call_count == 0);
        call_count = 42;
        ch.w << *call_count;
    });

    int child_val;
    ch.r >> child_val;
    assert(child_val == 42);

    // Parent's value is independent.
    assert(*call_count == 1);
});
csp::schedule();
```

---

## dynamic\<T\> vs imp\_local\<T\>

| Property | `dynamic<T>` | `imp_local<T>` |
|----------|-------------|---------------|
| Inherited by children | Yes (HAMT snapshot at spawn) | No |
| Write mechanism | `local` scope (RAII, path-copy) | Direct `operator=` |
| Isolation | Copy-on-write per scope | Per-imp map |
| Storage | Persistent HAMT (shared structure) | `unordered_map` per MT |
| Use case | Request context, trace IDs, config | Counters, caches, scratch state |

## Use outside an imp

All dynamic-scope APIs are storage-attached to the current imp. When
called from a thread that is not running an imp -- e.g. directly from
`main()` before any `csp::run` / `csp::spawn`, or from a foreign
thread that has never been bound to the CSP runtime -- behaviour is
defined as follows:

| Operation | Outside any imp |
|-----------|-----------------|
| `*var`, `var->...` on `dynamic<T>` / `imp_local<T>` | Returns the default value |
| `csp::local{var = val, ...}` | Throws `csp::error` |
| `imp_local<T>::operator=(val)` | Throws `csp::error` |
| `csp::context::current()` | Returns an empty `context` |
| `csp::context_scope(ctx)` | Throws `csp::error` |

Reads degrade gracefully because no binding can exist on a
non-existent imp -- the default is the only meaningful answer.
Scope-binding operations throw a `csp::error` whose message names the
API and points to `csp::run` / `csp::spawn`, so misuse from `main()`
fails with a clear diagnostic instead of a segfault.
