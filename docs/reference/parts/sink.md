# sink

Consumes all values from a stream by applying a side-effect function to each
one. `sinkhole` is a convenience variant that assigns each value to a
reference.

## Signature

```cpp
template <typename A, typename F>
auto sink(F&& f);
// Returns: consumer<A, ...>

template <typename T>
auto sinkhole(T& t);
// Returns: consumer<T, ...>
```

## Topology

```mermaid
graph LR
    A[reader&lt;A&gt;] --> B["sink(f)"]
```

One internal imp reads every value from the input channel and invokes
`f` on it. The imp exits when the input closes.

## Semantics

- Reads and processes every value until the input channel is exhausted.
- `f` is called exactly once per input element, in order.
- Backpressure: the imp blocks on each input read, so it consumes
  values as fast as the producer supplies them. There is no output channel to
  create downstream backpressure.
- `sinkhole(t)` is equivalent to `sink<T>([&t](T a) { t = a; })` -- it
  always holds the most recently received value.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Sum values via sink.
int total = 0;
auto w = sink<int>([&](int n) { total += n; }).spawn();
for (int i = 1; i <= 10; ++i) {
    w << i;
}
w = {};
schedule();
// total == 55

// Track the latest value via sinkhole.
int latest = 0;
auto w2 = sinkhole<int>(latest).spawn();
w2 << 1; w2 << 2; w2 << 3;
// latest == 3
```

## Pipe Composition

`sink` is a consumer, so it composes naturally with filters and producers
via the `|` operator:

```cpp
// Double each value, then sum.
int total = 0;
auto w = (map<int>([](int n) { return n * 2; })
        | sink<int>([&](int n) { total += n; }))
          .spawn();

// Fully composed pipeline (producer | consumer).
auto run_it = count(1, 4)
            | sink<int>([&](int n) { total += n; });
csp::spawn(std::move(run_it));
```

## See Also

- [blackhole](blackhole.md) -- discard all values (sink with no side effect)
- [map](map.md) -- transform elements before consuming
