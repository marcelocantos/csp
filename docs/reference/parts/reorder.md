# csp::part::reorder

Restores key order to an out-of-sequence stream. Elements are held in a
`std::map<Key, T>` buffer and emitted as soon as contiguous keys can be
flushed starting from `initial_key`. Memory usage is proportional to the
maximum out-of-order distance, not the total stream length.

> **Design note.** `reorder` assumes keys form a contiguous ascending
> sequence. It is designed for the common case of reassembling results from a
> parallel worker pool (e.g., `parallel_map` preceded by `enumerate`) where
> keys are sequential integers. Non-sequential or non-integer keys are
> supported as long as they form a contiguous ascending sequence under `++`.

## Signature

```cpp
template <typename T, typename Key = size_t>
auto reorder(std::function<Key(T const&)> key_fn, Key initial_key = Key{});
// Returns: filter<T, T, ...>
```

## Parameters

| Parameter | Type | Description |
|---|---|---|
| `key_fn` | `std::function<Key(T const&)>` | Extract a comparable, incrementable key from each element |
| `initial_key` | `Key` | Expected key of the first output element (default: `Key{}`) |

## Topology

<!-- csp-flow
reader<T> -> {reorder(key_fn)} -> reader<T>
-->
![reorder topology](diagrams/reorder.svg)

One internal imp reads elements, buffers out-of-order arrivals in a sorted
map, and flushes contiguous runs whenever the next expected key arrives.

## Semantics

- Maintains a `next` counter starting at `initial_key`.
- When an element with `key_fn(t) == next` arrives, it is emitted directly,
  `next` is incremented, and any buffered elements with newly contiguous keys
  are also flushed in order.
- When `key_fn(t) != next`, the element is buffered under its key.
- Duplicate keys overwrite the previously buffered value for that key.
- When the input closes, all remaining buffered elements are flushed in key
  order, even if keys are not contiguous from `next` (i.e., the gap is
  treated as if the missing elements never arrived).
- Exits after flushing buffered elements when the input closes, or immediately
  when the output reader is dropped.
- Backpressure: the imp blocks on each write, so a slow consumer throttles
  the pipeline.

## Example

```cpp
#include "csp.h"

#include <utility>

using namespace csp;
using namespace csp::part;

// Reassemble parallel_map results in original order.
using P = std::pair<size_t, Result>;
auto ordered = reorder<P>(
        std::function<size_t(P const&)>([](P const& p) { return p.first; }))
    .spawn(
        parallel_map<std::pair<size_t, Input>, P>(4, worker)
            .spawn(enumerate<Input>(inputs).spawn()));

for (P p; ordered >> p;) use(p.second);
```

## See Also

- [parallel_map](parallel_map.md) -- concurrent transform that may reorder results
- [enumerate](enumerate.md) -- attach sequential indices to stream elements
- [sort_merge](sort_merge.md) -- merge N independently-sorted streams into one sorted output
- [merge](merge.md) -- non-deterministic merge (no ordering guarantee)
