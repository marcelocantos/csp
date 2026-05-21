# csp::part::frame

Groups input into fixed-size frames, with a timeout to flush partial frames
when input is slow. Each full frame of `n` elements is emitted as a
`std::vector<T>`. When the timeout fires before `n` elements have accumulated,
the partial frame is flushed immediately. When the input closes mid-frame, the
partial frame is also flushed.

> **Design note.** `frame` adds a time dimension to `batch`: it guarantees
> data flows downstream even when the upstream rate is bursty or slower than
> the frame size. The combination of size and timeout makes it suitable for
> windowed aggregation and protocol framing over live streams. If you need
> pure size-driven grouping without timing, use `batch`.

## Signature

```cpp
template <typename T>
auto frame(size_t n, duration timeout);
// Returns: filter<T, std::vector<T>, ...>
```

## Parameters

| Parameter | Type | Description |
|---|---|---|
| `n` | `size_t` | Maximum number of elements per frame |
| `timeout` | `csp::duration` | Maximum wait time before flushing a partial frame |

## Topology

<!-- csp-flow
reader<T> -> {frame(n, timeout)} -> reader<vector<T>>
-->
![frame topology](diagrams/frame.svg)

One internal imp accumulates elements until `n` are collected or the timeout
fires, then emits the frame. The timeout is armed only when the buffer is
non-empty, so there is no spurious empty flush.

## Semantics

- Collects elements into a buffer until `n` are accumulated or the `timeout`
  fires, then emits a `vector<T>` with the buffered elements.
- The timeout timer is armed only after the first element of a new frame
  arrives. An empty buffer never produces an empty flush.
- When the input closes mid-frame, the partial frame is emitted before the
  output closes. A fully-empty input (channel closed before any element)
  produces no output.
- Exits when the input is exhausted (after flushing any partial frame) or when
  the output reader is dropped. On output death with a non-empty buffer, the
  remaining elements are discarded.
- Backpressure: the imp blocks on each vector write, so a slow consumer
  throttles the pipeline.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;
using namespace std::chrono_literals;

// At most 4 elements per frame; flush after 100 ms of silence.
auto r = frame<int>(4, 100ms).spawn(source);

std::vector<int> f;
while (r >> f) {
    process(f);
}
```

### Partial-frame flush on slow input

```cpp
// Source sends 2 elements then pauses for 200 ms.
// With timeout=50ms, the partial frame of 2 is emitted after 50 ms.
auto r = frame<int>(10, 50ms).spawn(slow_source);
```

## See Also

- [batch](batch.md) -- fixed-size grouping without timeout (emits only full frames)
- [chunk_by](chunk_by.md) -- predicate-driven grouping over adjacent pairs
- [debounce](debounce.md) -- emit only after a quiet period (drops intermediate values)
- [window](window.md) -- sliding window emitting the last N elements as a vector
