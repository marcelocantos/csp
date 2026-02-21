# buffer

Bounded (or unbounded) FIFO buffer that decouples a producer from a consumer.
Values are held in an internal ring buffer up to the given capacity, allowing
the producer to run ahead of the consumer.

## Signature

```cpp
template <typename T>
auto buffer(size_t capacity = size_t(-1));
// Returns: filter<T, T, ...>
```

## Topology

<!-- csp-flow
reader<T> -> {buffer(N)} -> reader<T>
-->
![buffer topology](diagrams/buffer.svg)

One internal imp manages a ring buffer between the input and output
channels, using `alt` to simultaneously offer reads and writes depending on
whether the buffer is full, empty, or neither.

## Semantics

- **Bounded mode** (`buffer<T>(N)`): the internal ring buffer holds at most
  `N` elements. When full, the imp stops reading from the input until
  a consumer takes a value, providing backpressure to the producer.
- **Unbounded mode** (`buffer<T>()`): capacity defaults to `size_t(-1)`,
  effectively unlimited. The producer is never blocked by buffer fullness
  (though it may still block on the synchronous channel write into the buffer).
- **Draining**: when the input channel closes, any remaining buffered values
  are drained to the output before the buffer imp exits.
- **Output closes**: if the output reader is dropped, the buffer imp
  exits immediately (buffered values are discarded).
- Order is strictly FIFO.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

// Bounded buffer with capacity 5.
auto ch = buffer<int>(5).spawn();

// Producer can write up to 5 values before blocking.
spawn([out = std::move(ch.w)] {
    for (int i = 1; i <= 10; ++i) {
        out << i;
    }
});

// Consumer reads all 10 values.
spawn([in = std::move(ch.r)] {
    for (int n; in >> n;) {
        // process n
    }
});

// Unbounded buffer.
auto ch2 = buffer<int>().spawn();
```

## See Also

- [latch](latch.md) -- holds the most recent value (capacity-1 buffer that overwrites)
- [sink](sink.md) -- consume values with a side-effect function
