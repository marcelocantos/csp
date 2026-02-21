# switch_all

Flatten a stream of sub-streams with latest-wins cancellation.
`reader<reader<B>>` becomes `reader<B>`. When a new sub-stream arrives, the
previous one is cancelled (reader dropped). Only the most recent sub-stream
is active at any time.

Compose with `map` for switch_map semantics:
`map<A, reader<B>>(f) | switch_all<B>`.

**Header:** `<csp/part/switch_all.h>`

## Synopsis

```cpp
template <typename B>
inline auto const switch_all = /* filter<reader<B>, B> */;
```

Returns a `filter<reader<B>, B>`. Used as `switch_all<int>` (variable
template, not a function call).

## Topology

<!-- csp-flow
reader<reader<B>> -> {switch_all} -> reader<B>
-->
![switch_all topology](diagrams/switch_all.svg)

## Semantics

- Reads the first sub-stream from input and begins forwarding its values.
- When a new sub-stream arrives, the old reader is dropped (cancelling the
  previous sub-stream) and the new one becomes active.
- When the active sub-stream dies naturally, waits for the next from input.
- When input closes, drains the remaining active sub-stream and exits.
- On output death, the imp exits immediately.

## Usage

```cpp
using namespace csp::part;

// switch_map: map each value to a sub-stream, cancel previous.
auto r = source.spawn()
       | map<int, reader<int>>([](int n) { return make_substream(n); })
       | switch_all<int>;
```

## See also

- [concat_all](concat_all.md) -- sequential drain (no cancellation)
- [exhaust_all](exhaust_all.md) -- ignore new while draining
- [flat_map](flat_map.md) -- concurrent merge of sub-streams
