# blackhole

Consumes and discards all values from a stream. Useful when a writer endpoint
must exist (to keep the producer alive) but the values are not needed.

## Signature

```cpp
template <typename T>
inline auto const blackhole;
// Type: consumer<T, ...>
```

`blackhole` is a `const` variable template, not a function. Use
`blackhole<T>.spawn()` to create the writer endpoint.

## Topology

```mermaid
graph LR
    A[reader&lt;T&gt;] --> B["blackhole"]
```

One internal microthread reads and discards every value until the input closes.

## Semantics

- Reads every value from the input channel and immediately drops it.
- Exits when the input channel is exhausted (all writers have closed).
- Backpressure: the microthread reads as fast as possible, so producers are
  never throttled by the consumer side. The only backpressure comes from the
  synchronous channel rendezvous itself.

## Example

```cpp
#include <csp/csp.h>
#include <csp/part/blackhole.h>

using namespace csp;
using namespace csp::part;

// Discard all output from a producer.
auto w = blackhole<int>.spawn();
for (int i = 0; i < 1000; ++i) {
    w << i;  // values are consumed and discarded
}
```

## When to Use

- **Testing**: provide a consumer endpoint when you only care about the
  producer side of a pipeline.
- **Side-effect producers**: when a microthread produces values as a side
  effect but no downstream consumer is needed.
- **Pipeline draining**: attach to a channel to ensure it drains completely,
  preventing upstream writers from blocking.

## See Also

- [sink](sink.md) -- consume values with a side-effect function
- [deaf](deaf.md) -- writer endpoint that never accepts values (opposite direction)
- [mute](mute.md) -- reader endpoint that never produces values
