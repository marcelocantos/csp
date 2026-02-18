# window

Sliding window that emits the full window contents as a vector after every
input element. `window<T>(n)` maintains a sliding window of at most `n`
elements and outputs a `std::vector<T>` snapshot on each step.

Partial (growing) windows are emitted during the initial fill phase, so the
first output has one element, the second has two, and so on up to `n`.

## Signature

```cpp
template <typename T>
auto window(size_t n);
// Returns: filter<T, std::vector<T>, ...>
```

## Topology

```mermaid
graph LR
    A[reader&lt;T&gt;] --> B["window(n)"] --> C["reader&lt;vector&lt;T&gt;&gt;"]
```

One internal microthread maintains a deque, copying it to a vector for each
output.

## Semantics

- Emits one vector per input element. The vector contains the last
  `min(elements_seen, n)` elements.
- During the growth phase (fewer than `n` elements seen), shorter vectors
  are emitted.
- When the window is full, the oldest element is dropped before adding
  the new one.
- Exits when the input is exhausted or the output reader is dropped.
- Backpressure: the microthread blocks on each vector write, so a slow
  consumer throttles the pipeline.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Window of 3 over 1..6.
auto r = window<int>(3).spawn(count(1, 7).spawn());

r.read(); // {1}
r.read(); // {1, 2}
r.read(); // {1, 2, 3}
r.read(); // {2, 3, 4}
r.read(); // {3, 4, 5}
r.read(); // {4, 5, 6}
```

## See Also

- [batch](batch.md) -- non-overlapping fixed-size grouping
- [slide](slide.md) -- two-channel window with separate enter/leave streams
- [nwise](nwise.md) -- sliding window emitting fixed-size tuples
