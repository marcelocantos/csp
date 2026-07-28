# csp::part::merge_all

Flatten a stream of sub-streams by merging all concurrently.
`reader<reader<B>>` becomes `reader<B>`. Every sub-stream joins the merge as
soon as it arrives; outputs from different sub-streams interleave
non-deterministically. The fourth flatten strategy, alongside
[concat_all](concat_all.md), [switch_all](switch_all.md), and
[exhaust_all](exhaust_all.md).

Compose with `map` for flat_map semantics:
`map<A, reader<B>>(f) | merge_all<B>` behaves like
[flat_map](flat_map.md)`<A, B>(f)`.

**Header:** `<csp/part/merge_all.h>`

## Synopsis

```cpp
template <typename B>
inline auto const merge_all = /* filter<reader<B>, B> */;
```

Returns a `filter<reader<B>, B>`. Used as `merge_all<int>` (variable
template, not a function call).

## Topology

<!-- csp-flow
reader<reader<B>> -> {merge_all} -> reader<B>
-->
![merge_all topology](diagrams/merge_all.svg)

One internal imp multiplexes the input and every active sub-stream through a
single dynamic alt.

## Semantics

- New sub-streams are accepted while existing ones drain; all active
  sub-streams compete in one alt, so output order across sub-streams is
  non-deterministic. Intra-stream order is preserved.
- A sub-stream that dies is removed from the merge set.
- On input close, active sub-streams keep draining; the output closes when
  the last one closes.
- On output death, the imp exits immediately (pending sub-streams are
  dropped).
- Backpressure: each forwarded value is a synchronous rendezvous with the
  output reader; sub-streams that are not selected block on their writers.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

csp::run([]{
    auto [w, r] = chan<reader<int>>{};
    auto out = merge_all<int>.spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        w << count(10, 13).spawn();
        w << count(20, 23).spawn();
    });

    std::vector<int> got;
    for (int n; out >> n;) got.push_back(n);
    // got holds {10, 11, 12} and {20, 21, 22} interleaved in some order.
});
```

## See Also

- [concat_all](concat_all.md) -- drain each sub-stream sequentially
- [switch_all](switch_all.md) -- latest-wins cancellation
- [exhaust_all](exhaust_all.md) -- ignore new while draining
- [flat_map](flat_map.md) -- map + merge_all in one part
- [merge](merge.md) -- merge a fixed vector of readers
