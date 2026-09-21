# csp::part::rand::random_bytes

Infinite stream of random byte chunks of a fixed size. A producer: it takes
no input and writes `bytes` (`std::vector<uint8_t>`) messages of exactly
`chunk_size` bytes forever, until the downstream reader goes away. Useful as
a load generator for byte pipelines -- socket writers, framers, hashers --
where the content is irrelevant but the shape is not.

Unlike its siblings in [random](random.md), `random_bytes` refills and
re-sends one shared buffer rather than drawing a fresh value per send.

## Signature

```cpp
template <typename Engine = std::mt19937_64>
auto random_bytes(size_t chunk_size,
                  Engine eng = Engine{std::random_device{}()});
// Returns: producer<bytes>
```

**Header:** `#include "csp.h"`

## Parameters

| Parameter | Type | Description |
|-----------|------|-------------|
| `chunk_size` | `size_t` | Number of bytes in every emitted chunk |
| `eng` | `Engine` | Random engine; defaults to `std::mt19937_64` seeded from `std::random_device` |

## Topology

<!-- csp-flow
{random_bytes(chunk_size)} -> reader<bytes>
-->
![random_bytes topology](diagrams/random_bytes.svg)

No input channel. One internal imp fills a buffer with
`std::uniform_int_distribution<unsigned>(0, 255)` draws and writes it to the
output, then refills the same buffer for the next send.

## Semantics

- **Chunk size is exact**: every message is a `bytes` of precisely
  `chunk_size` elements -- unlike [byte_reader](byte_reader.md), whose chunk
  size is an upper bound set by what the kernel returned.
- **Fresh bytes per chunk**: the buffer is refilled before each send, so
  consecutive chunks are independent draws. The buffer is sent as an lvalue,
  so each receiver gets its own copy and the producer keeps the storage.
- **Infinite**: the stream never ends on its own; there is no input channel
  to close and no bound on the number of chunks.
- **Backpressure**: each write blocks until a reader is ready (synchronous
  channel semantics). No buffering, and no bytes are generated while blocked
  -- the refill happens after the previous send completes.
- **Exit**: the imp exits, closing its writer, as soon as a send fails
  because the downstream reader was destroyed.
- **Empty chunks**: `chunk_size == 0` is legal and emits empty `bytes`
  forever -- a busy loop against whatever consumes it.
- **Reproducibility**: pass a seeded engine to get a deterministic stream.
- **Not cryptographic**: `std::mt19937_64` is not a secure generator. Do not
  use `random_bytes` for keys, nonces or tokens.

## Example

```cpp
#include "csp.h"

using namespace csp::part;

// 64-byte chunks of random data.
auto r = rand::random_bytes(64).spawn();

auto chunk = r.read();   // chunk.size() == 64
auto next  = r.read();   // a different 64 bytes

// Dropping the reader terminates the producer.
r = {};
```

Feeding a byte pipeline, with a seeded engine so the run repeats exactly:

```cpp
// Frame a reproducible random stream into 512-byte frames.
auto r = (rand::random_bytes(4096, std::mt19937_64{42})
    | io::fixed_frames(512)).spawn();
```

## See Also

- [random](random.md) -- the rest of `csp::part::rand` (distributions,
  `choice`, `shuffle`)
- [byte_reader](byte_reader.md) -- byte chunks from a file descriptor
- [fixed_frames](fixed_frames.md) -- re-chunk a byte stream into fixed frames
- [count](count.md) -- deterministic source for the same shape of test
