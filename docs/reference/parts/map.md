# map

Transforms each element of a stream by applying a function. `map<A, B>(f)`
converts a `reader<A>` into a `reader<B>`. When the input and output types are
the same, the second template parameter can be omitted.

## Signature

```cpp
template <typename A, typename B = A, typename F>
auto map(F&& f);
// Returns: filter<A, B, ...>
```

## Topology

```mermaid
graph LR
    A[reader&lt;A&gt;] --> B["map(f)"] --> C[reader&lt;B&gt;]
```

One internal microthread reads from the input, applies `f`, and writes the
result to the output.

## Semantics

- Exits when the input is exhausted or the output reader is dropped.
- Backpressure: the microthread blocks on each output write, so a slow
  consumer throttles the entire pipeline.
- Each input value is passed by value to `f`. The result of `f` is written
  to the output channel.
- `f` is invoked exactly once per input element.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Same-type transform: increment each integer.
auto r = map<int>([](int n) { return n + 1; })
             .spawn(count(1, 4).spawn());
// Reads: 2, 3, 4

// Type-changing transform: string length.
auto r2 = map<std::string, size_t>([](auto&& s) { return s.length(); })
              .spawn(enumerate<std::string>({"hi", "hello"}).spawn());
// Reads: 2, 5
```

## See Also

- [where](where.md) -- filter elements by predicate
- [scan](scan.md) -- running fold with intermediate results
- [flat_map](flat_map.md) -- map each element to a sub-stream, then merge
