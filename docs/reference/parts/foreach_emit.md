# foreach_emit

Generalized stateful transform with separate update and extraction functions.
Maintains an internal state that is updated on each input element, and emits
a value derived from the state after each step. Unlike `scan`, the state type,
input type, and output type can all differ.

## Signature

```cpp
template <typename T, typename S, typename U, typename Update, typename Extract>
auto foreach_emit(S init, Update&& update, Extract&& extract);
// Returns: filter<T, U, ...>
```

## Topology

<!-- csp-flow
reader<T> -> {foreach_emit(init, update, extract)} -> reader<U>
-->
![foreach_emit topology](diagrams/foreach_emit.svg)

One internal imp maintains the state, reads each input, applies `update`
to advance the state, then writes `extract(state)` to the output.

## Semantics

- For each input element `t`, the state is advanced:
  `state = update(std::move(state), t)`.
- After each update, `extract(state)` is called and its result is written to
  the output.
- Emits one output for every input element.
- The state type `S`, input type `T`, and output type `U` can all be different,
  allowing flexible decoupling of internal bookkeeping from the emitted stream.
- Exits when the input is exhausted or the output reader is dropped.
- Backpressure: blocks on each output write before consuming the next input.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Running average: state is (sum, count), output is the double average.
auto r = foreach_emit<int, std::pair<int,int>, double>(
    {0, 0},
    [](std::pair<int,int> s, int v) {
        return std::pair{s.first + v, s.second + 1};
    },
    [](const std::pair<int,int>& s) {
        return (double)s.first / s.second;
    })
    .spawn(count(1, 5).spawn());
// Reads: 1.0, 1.5, 2.0, 2.5
```

## See Also

- [scan](scan.md) -- running fold where state and output are the same type
- [reduce](reduce.md) -- fold to a single final value
