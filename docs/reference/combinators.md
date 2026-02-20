# Combinators Reference

The **combinator framework** (`namespace csp::part`) provides typed wrappers
for composing channel pipelines. Three struct templates -- `producer`, `filter`,
and `consumer` -- represent the three fundamental pipeline roles. The `|` pipe
operator composes them into larger structures, and factory functions create them
from plain callables.

All combinator types live in `namespace csp::part`. The immediate-spawn helpers
`spawn_producer`, `spawn_consumer`, and `spawn_filter` live in `namespace csp`.

Header: `#include "csp.h"`

---

## Table of Contents

1. [producer\<T, F\>](#producert-f) -- writer-producing wrapper
2. [filter\<In, Out, F\>](#filterin-out-f) -- reader-to-writer transform wrapper
3. [consumer\<T, F\>](#consumert-f) -- reader-consuming wrapper
4. [make_producer, make_filter, make_consumer](#factory-functions) -- factory functions
5. [Pipe operator (|)](#pipe-operator) -- composition via operator|
6. [spawn_producer, spawn_consumer, spawn_filter](#immediate-spawn-helpers) -- immediate-spawn helpers

---

## producer\<T, F\>

A lazy wrapper holding a body function that writes values to a `writer<T>`.
Nothing executes until `spawn()` is called.

### Signature

```cpp
template <typename T, typename F>
struct producer {
    F body_;

    void operator()(writer<T> w);

    auto bind(writer<T> w) const &;    // returns callable
    auto bind(writer<T> w) &&;         // returns callable (moves body)

    reader<T> spawn() const &;         // copies body into imp
    reader<T> spawn() &&;              // moves body into imp
};
```

### Description

`producer` wraps a callable `F` that accepts a `writer<T>` and produces values
by writing to it. The wrapper provides three ways to use the body:

**`operator()(writer<T>)`** runs the body inline, passing the writer directly.
The call blocks until the body returns.

**`bind(writer<T>)`** captures the body and writer into a deferred callable
(a lambda with no arguments). Calling the returned lambda runs the body with
the captured writer. The `const &` overload copies the body; the `&&` overload
moves it.

**`spawn()`** creates a new channel, spawns an imp that runs the body
with the write end, and returns the read end. This is the primary way to use a
producer -- it wires up the channel plumbing and launches execution. The
`const &` overload copies the body into the imp; the `&&` overload
moves it, enabling move-only captures (e.g., a `std::vector<reader<T>>`
captured by a merge combinator).

### Transition rules ([syntax](transition-rules.md))

```
producer.spawn()  ────────────────➤ chan<T> created; imp M spawned;
                                    M calls body_(writer<T>);
                                    → reader<T>

body_ returns     ────────────────➤ writer<T> closed; reader sees dead channel
```

### Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

spawn([] {
    auto p = make_producer<int>([](writer<int> w) {
        for (int i = 0; i < 5; ++i)
            w << i;
    });

    // Nothing has executed yet -- p is lazy.
    auto r = std::move(p).spawn();

    // Now an imp is running, producing 0..4.
    for (int v : r) {
        // v: 0, 1, 2, 3, 4
    }
});
schedule();
```

---

## filter\<In, Out, F\>

A lazy wrapper holding a body function that transforms a `reader<In>` into a
`writer<Out>`. Provides multiple `spawn` overloads for binding one or both
endpoints.

### Signature

```cpp
template <typename In, typename Out, typename F>
struct filter {
    F body_;

    void operator()(reader<In> r, writer<Out> w);

    auto bind(reader<In> r, writer<Out> w) const &;   // returns callable
    auto bind(reader<In> r, writer<Out> w) &&;         // returns callable (moves body)

    writer<In> spawn(writer<Out> w) const &;    // bind output, return input writer
    writer<In> spawn(writer<Out> w) &&;

    reader<Out> spawn(reader<In> r) const &;    // bind input, return output reader
    reader<Out> spawn(reader<In> r) &&;

    // Only available when In == Out:
    template <typename T = In>
    chan<T> spawn() const &;
    template <typename T = In>
    chan<T> spawn() &&;
};
```

### Description

`filter` wraps a callable `F` that accepts `reader<In>` and `writer<Out>` and
transforms a stream of `In` values into a stream of `Out` values. The wrapper
provides several ways to wire the filter into a pipeline:

**`operator()(reader<In>, writer<Out>)`** runs the body inline with both
endpoints. The call blocks until the body returns.

**`bind(reader<In>, writer<Out>)`** captures the body and both endpoints into
a deferred callable. The `const &` overload copies the body; the `&&` overload
moves it.

**`spawn(writer<Out>)`** binds the output end, creating a new input channel.
The returned `writer<In>` is the input endpoint; the caller writes values into
it and the filter's imp reads them and writes transformed results to
the provided `writer<Out>`.

**`spawn(reader<In>)`** binds the input end, creating a new output channel.
The returned `reader<Out>` is the output endpoint. This is the most common
usage pattern -- pipe an existing reader through a filter and get back a
transformed reader:

```cpp
auto doubled = map<int>([](int n) { return n * 2; }).spawn(source);
```

**`spawn()`** (only when `In == Out`) creates both an input channel and an
output channel, returning a `chan<T>` whose `.w` accepts input and whose `.r`
emits output.

All `spawn` overloads launch an imp. The `const &` overloads copy the
body; the `&&` overloads move it.

### Transition rules ([syntax](transition-rules.md))

```
filter.spawn(reader<In>)   ──────➤ chan<Out> created; imp M spawned;
                                   M calls body_(reader<In>, writer<Out>);
                                   → reader<Out>

filter.spawn(writer<Out>)  ──────➤ chan<In> created; imp M spawned;
                                   M calls body_(reader<In>, writer<Out>);
                                   → writer<In>

filter.spawn()             ──────➤ chan<T> in + chan<T> out created;
                                   imp M spawned;
                                   M calls body_(in.reader, out.writer);
                                   → chan<T>{in.writer, out.reader}

body_ returns              ──────➤ endpoints closed; peers see dead channel
```

### Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

spawn([] {
    // Create a filter that squares integers.
    auto sq = make_filter<int>([](reader<int> r, writer<int> w) {
        for (int v : r)
            w << v * v;
    });

    // Bind input reader, get output reader.
    auto source = count(1, 6).spawn();
    auto squared = std::move(sq).spawn(std::move(source));

    for (int v : squared) {
        // v: 1, 4, 9, 16, 25
    }
});
schedule();
```

---

## consumer\<T, F\>

A lazy wrapper holding a body function that reads values from a `reader<T>`.
Nothing executes until `spawn()` is called.

### Signature

```cpp
template <typename T, typename F>
struct consumer {
    F body_;

    void operator()(reader<T> r);

    auto bind(reader<T> r) const &;    // returns callable
    auto bind(reader<T> r) &&;         // returns callable (moves body)

    writer<T> spawn() const &;         // copies body into imp
    writer<T> spawn() &&;              // moves body into imp
};
```

### Description

`consumer` wraps a callable `F` that accepts a `reader<T>` and consumes a
stream of values. The wrapper provides three ways to use the body:

**`operator()(reader<T>)`** runs the body inline, passing the reader directly.
The call blocks until the body returns.

**`bind(reader<T>)`** captures the body and reader into a deferred callable.
The `const &` overload copies the body; the `&&` overload moves it.

**`spawn()`** creates a new channel, spawns an imp that runs the body
with the read end, and returns the write end. The caller writes values into the
returned `writer<T>` and the consumer's imp reads and processes them.

### Transition rules ([syntax](transition-rules.md))

```
consumer.spawn()  ────────────────➤ chan<T> created; imp M spawned;
                                    M calls body_(reader<T>);
                                    → writer<T>

caller closes writer  ───────────➤ reader sees dead channel; body_ can exit
```

### Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

spawn([] {
    int total = 0;
    auto c = make_consumer<int>([&total](reader<int> r) {
        for (int v : r)
            total += v;
    });

    auto w = std::move(c).spawn();
    for (int i = 1; i <= 5; ++i)
        w << i;
    w = {};  // close writer, consumer exits

    // total == 15
});
schedule();
```

---

## Factory Functions

Factory functions that wrap a callable in the appropriate combinator struct.

### Signature

```cpp
template <typename T, typename F>
producer<T, std::decay_t<F>> make_producer(F&& f);

template <typename In, typename Out = In, typename F>
filter<In, Out, std::decay_t<F>> make_filter(F&& f);

template <typename T, typename F>
consumer<T, std::decay_t<F>> make_consumer(F&& f);
```

### Description

These functions construct `producer`, `filter`, or `consumer` wrappers from a
callable. The callable is decay-copied (or moved) into the wrapper's `body_`
field.

**`make_producer<T>(f)`** requires `f` to be callable as `f(writer<T>)`.

**`make_filter<In, Out>(f)`** requires `f` to be callable as
`f(reader<In>, writer<Out>)`. When `In` and `Out` are the same type, the
second template parameter can be omitted: `make_filter<int>(f)`.

**`make_consumer<T>(f)`** requires `f` to be callable as `f(reader<T>)`.

All three return the corresponding wrapper struct. The wrappers are lazy --
the callable does not execute until `operator()`, `bind()`, or `spawn()` is
used.

### Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Create lazy combinators.
auto source = make_producer<int>([](writer<int> w) {
    w << 1; w << 2; w << 3;
});

auto doubled = make_filter<int>([](reader<int> r, writer<int> w) {
    for (int v : r) w << v * 2;
});

auto sink = make_consumer<int>([](reader<int> r) {
    for (int v : r) { /* process v */ }
});

// Compose and run.
spawn(source | doubled | sink);
schedule();
```

---

## Pipe Operator

The `|` operator composes combinators into larger structures. Eight overloads
cover all useful combinations of `producer`, `filter`, `consumer`, `reader`,
and `writer`.

### Overloads

| Left | Right | Result | Spawns? |
|------|-------|--------|---------|
| `filter<In,Mid>` | `filter<Mid,Out>` | `filter<In,Out>` | No |
| `producer<T>` | `filter<T,Out>` | `producer<Out>` | No |
| `filter<In,Out>` | `consumer<Out>` | `consumer<In>` | No |
| `producer<T>` | `consumer<T>` | callable `()` | No |
| `reader<In>` | `filter<In,Out>` | `reader<Out>` | Yes |
| `reader<T>` | `consumer<T>` | callable `()` | No |
| `filter<In,Out>` | `writer<Out>` | `writer<In>` | Yes |
| `producer<T>` | `writer<T>` | callable `()` | No |

### Description

The pipe operator uses `std::move` on the left operand throughout, enabling
move-only captures in combinator bodies.

**Lazy compositions** (combinator | combinator) produce a new combinator of the
appropriate type. The composed body, when eventually spawned, chains the
intermediate channels internally. No imps are created until `spawn()`
is called on the result.

**Immediate compositions** (reader | combinator, combinator | writer) call
`spawn()` on the right-hand combinator immediately, wiring the live endpoint
into the pipeline. These return a live endpoint (`reader` or `writer`) that is
already connected to a running imp.

**Terminal compositions** (producer | consumer, reader | consumer,
producer | writer) produce a plain callable that, when invoked, runs the
entire pipeline.

### Signatures

```cpp
// Lazy compositions (no spawn).
template <typename In, typename Mid, typename F1, typename Out, typename F2>
auto operator|(filter<In, Mid, F1> lhs, filter<Mid, Out, F2> rhs);
// → filter<In, Out, ...>

template <typename T, typename F1, typename Out, typename F2>
auto operator|(producer<T, F1> lhs, filter<T, Out, F2> rhs);
// → producer<Out, ...>

template <typename In, typename Out, typename F1, typename F2>
auto operator|(filter<In, Out, F1> lhs, consumer<Out, F2> rhs);
// → consumer<In, ...>

template <typename T, typename F1, typename F2>
auto operator|(producer<T, F1> lhs, consumer<T, F2> rhs);
// → callable ()

// Immediate compositions (spawn on construction).
template <typename In, typename Out, typename F>
reader<Out> operator|(reader<In> r, filter<In, Out, F> f);
// → reader<Out>

template <typename In, typename Out, typename F>
writer<In> operator|(filter<In, Out, F> f, writer<Out> w);
// → writer<In>

// Terminal compositions (returns callable).
template <typename T, typename F>
auto operator|(reader<T> r, consumer<T, F> c);
// → callable ()

template <typename T, typename F>
auto operator|(producer<T, F> p, writer<T> w);
// → callable ()
```

### Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

spawn([] {
    // Lazy composition: producer | filter | consumer → callable.
    auto pipeline = count(1, 6)
        | map<int>([](int n) { return n * n; })
        | sink<int>([](reader<int> r) {
            for (int v : r) { /* 1, 4, 9, 16, 25 */ }
          });
    pipeline();  // runs the whole pipeline

    // Immediate composition: reader | filter → reader.
    auto source = count(1, 4).spawn();  // reader<int>
    auto doubled = std::move(source)
        | map<int>([](int n) { return n * 2; });
    // doubled is a reader<int> backed by a running imp.
    for (int v : doubled) {
        // v: 2, 4, 6
    }
});
schedule();
```

---

## Immediate-Spawn Helpers

Convenience functions that create a channel, spawn an imp, and return
the external endpoint in one step. These are not lazy -- the imp starts
immediately.

### spawn_producer

```cpp
template <typename T, typename F>
reader<T> spawn_producer(F&& f);
```

Creates a `chan<T>`, spawns an imp that calls `f(writer<T>)` with the
write end, and returns the read end. The imp owns the writer; when `f`
returns, the write end closes and the reader sees channel death.

### spawn_consumer

```cpp
template <typename T, typename F>
writer<T> spawn_consumer(F f);
```

Creates a `chan<T>`, spawns an imp that calls `f(reader<T>)` with the
read end, and returns the write end. The caller writes values into the returned
writer; when the caller closes it, the reader sees channel death and `f` can
exit.

### spawn_filter

```cpp
template <typename T, typename F>
chan<T> spawn_filter(F&& f);
```

Creates two channels (input and output), spawns an imp that calls
`f(reader<T>, writer<T>)`, and returns a `chan<T>` whose `.w` is the input
writer and `.r` is the output reader.

### Transition rules ([syntax](transition-rules.md))

```
spawn_producer<T>(f) ────────────➤ chan<T> created; M spawned;
                                   M calls f(writer<T>);
                                   → reader<T>

spawn_consumer<T>(f) ────────────➤ chan<T> created; M spawned;
                                   M calls f(reader<T>);
                                   → writer<T>

spawn_filter<T>(f)   ────────────➤ in chan<T> + out chan<T> created; M spawned;
                                   M calls f(in.reader, out.writer);
                                   → chan<T>{in.writer, out.reader}
```

### Example

```cpp
#include "csp.h"

using namespace csp;

spawn([] {
    // spawn_producer: immediate imp, returns reader.
    auto r = spawn_producer<int>([](writer<int> w) {
        for (int i = 0; i < 3; ++i) w << i;
    });

    // spawn_consumer: immediate imp, returns writer.
    auto w = spawn_consumer<int>([](reader<int> r) {
        for (int v : r) { /* process v */ }
    });

    // Connect them.
    for (int v : r) w << v;
});
schedule();
```
