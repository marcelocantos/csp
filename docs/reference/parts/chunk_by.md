# csp::part::chunk_by

Groups consecutive elements into vectors based on a binary predicate over
adjacent pairs. A new chunk starts whenever `f(prev, curr)` returns false.
Each chunk is emitted as a `std::vector<T>`.

## Signature

```cpp
template <typename T, typename F>
auto chunk_by(F&& f);
// Returns: filter<T, std::vector<T>, ...>
```

## Topology

<!-- csp-flow
reader<T> -> {chunk_by(f)} -> reader<vector<T>>
-->
![chunk_by topology](diagrams/chunk_by.svg)

One internal imp reads from the input, accumulates elements into a
chunk while `f(prev, curr)` returns true, and emits each completed
chunk as a vector.

## Semantics

- Accumulates consecutive elements where `f(prev, curr)` is true into the
  current chunk.
- When `f(prev, curr)` returns false, the current chunk is emitted and a new
  chunk is started with `curr` as its first element.
- The final chunk is flushed when the input is exhausted, even if it was
  never terminated by a false return from `f`.
- An empty input produces no output (no empty vectors are emitted).
- A single-element input emits one single-element vector.
- `f` receives const references to the last element of the current chunk and
  the incoming element.
- Backpressure: the imp blocks on each vector write, so a slow consumer
  throttles the pipeline.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Group consecutive elements that differ by at most 1.
auto r = chunk_by<int>([](int a, int b) { return b - a <= 1; })
             .spawn(enumerate<int>({1, 2, 3, 7, 8, 20}).spawn());
// Reads: {1, 2, 3}, {7, 8}, {20}
```

## See Also

- [batch](batch.md) -- fixed-size grouping (ignores element values)
- [group_by](group_by.md) -- dynamic partitioning by key (non-consecutive groups)
