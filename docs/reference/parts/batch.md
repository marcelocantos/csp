# csp::part::batch

Collects elements into fixed-size vectors and emits each vector as a single
value. `batch<T>(n)` groups every `n` input elements into a
`std::vector<T>`. Any partial batch remaining when the input closes is
flushed as a shorter vector.

## Signature

```cpp
template <typename T>
auto batch(size_t n);
// Returns: filter<T, std::vector<T>, ...>
```

## Topology

<!-- csp-flow
reader<T> -> {batch(n)} -> reader<vector<T>>
-->
![batch topology](diagrams/batch.svg)

One internal imp accumulates up to `n` elements, then writes the
vector to the output channel.

## Semantics

- Emits a full vector of `n` elements each time the buffer fills.
- When the input closes with a non-empty partial buffer, that buffer is
  flushed as a final, shorter vector.
- When the input closes on an exact batch boundary, no trailing empty
  vector is emitted.
- Exits when the input is exhausted (after flushing) or the output reader
  is dropped.
- Backpressure: the imp blocks on each vector write, so a slow
  consumer throttles the pipeline.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// 10 elements in batches of 3 -> [1,2,3], [4,5,6], [7,8,9], [10]
auto r = batch<int>(3).spawn(count(1, 11).spawn());

r.read(); // {1, 2, 3}
r.read(); // {4, 5, 6}
r.read(); // {7, 8, 9}
r.read(); // {10}        -- partial final batch
```

## See Also

- [window](window.md) -- sliding window emitting the full window each step
- [nwise](nwise.md) -- sliding N-element window emitting tuples
- [flatten](flatten.md) -- inverse of batch: expand vectors back to elements
