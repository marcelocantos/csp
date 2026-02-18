# nwise

Sliding N-element window that emits tuples. `nwise<N, T>()` collects a
window of exactly `N` consecutive elements and emits each window as a
`std::tuple<T, T, ..., T>` (N copies of `T`). The window slides by one
element per step.

If the input stream has fewer than `N` elements, no output is produced.

## Signature

```cpp
template <size_t N, typename T>
auto nwise();
// Returns: filter<T, std::tuple<T, T, ..., T>, ...>
// Requires: N >= 2
```

## Topology

```mermaid
graph LR
    A[reader&lt;T&gt;] --> B["nwise&lt;N&gt;()"] --> C["reader&lt;tuple&lt;T,...&gt;&gt;"]
```

One internal microthread maintains a fixed-size array that slides through
the input.

## Semantics

- Waits for the first `N` elements before emitting anything.
- After the initial fill, emits one tuple per input element by shifting the
  window left and reading into the last slot.
- If the input has fewer than `N` elements, the output closes immediately
  with no values.
- Exits when the input is exhausted or the output reader is dropped.
- Backpressure: the microthread blocks on each tuple write, so a slow
  consumer throttles the pipeline.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Sliding triples over 1..6.
auto r = nwise<3, int>().spawn(count(1, 7).spawn());

r.read(); // tuple(1, 2, 3)
r.read(); // tuple(2, 3, 4)
r.read(); // tuple(3, 4, 5)
r.read(); // tuple(4, 5, 6)
```

## See Also

- [pairwise](pairwise.md) -- specialized `nwise<2>` returning `std::pair`
- [window](window.md) -- sliding window emitting variable-size vectors
- [batch](batch.md) -- non-overlapping fixed-size grouping
