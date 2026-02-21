# split_lines

Splits a byte stream into newline-delimited strings. Pure channel transform
with no I/O knowledge -- testable with synthetic data.

## Signature

```cpp
inline auto const split_lines = /* ... */;
// Returns: filter<std::vector<uint8_t>, std::string, ...>
```

## Topology

<!-- csp-flow
reader<vector<uint8_t>> -> split_lines -> reader<string>
-->
![split_lines topology](diagrams/split_lines.svg)

One internal imp reads byte chunks from the input, scans for `'\n'`
characters, and emits one `std::string` per complete line.

## Semantics

- **Delimiter**: Lines are split on `'\n'` (LF). The newline character is not
  included in the output string.
- **Chunk reassembly**: Line boundaries may fall anywhere within or across
  input chunks. The filter maintains an internal buffer (`pending`) that
  accumulates partial line data across chunk boundaries.
- **Trailing data flush**: When the input channel closes, any remaining data
  in the pending buffer is emitted as a final line (even without a trailing
  newline). This ensures no data is silently lost.
- **Empty lines**: An input `"\n\n"` produces two empty strings.
- **Output close**: Uses `csp::alt(in >> chunk, ~out)` to detect downstream
  reader drop. If the output reader is dropped, the filter exits immediately
  without draining the input.
- **Backpressure**: The imp blocks on each output write, so a slow
  consumer throttles the entire pipeline.
- **No I/O dependency**: `split_lines` operates purely on channels. It can be
  tested with synthetic `chan<std::vector<uint8_t>>` data or composed with
  `byte_reader` for real I/O.

## Example

### Standalone (synthetic data)

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

auto [w, r] = chan<std::vector<uint8_t>>{};
auto lr = io::split_lines.spawn(std::move(r));

csp::spawn([w = std::move(w)] {
    std::string data = "hello\nworld\nfoo\n";
    std::vector<uint8_t> v(data.begin(), data.end());
    w << std::move(v);
});

// Reads: "hello", "world", "foo"
```

### Composed with byte_reader

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

int pipefd[2];
pipe(pipefd);

// Compose: fd -> byte chunks -> lines
auto lr = io::split_lines.spawn(io::byte_reader(pipefd[0]).spawn());

csp::spawn([wfd = pipefd[1]] {
    const char* data = "alpha\nbeta\ngamma\n";
    ::write(wfd, data, strlen(data));
    ::close(wfd);
});

std::vector<std::string> result;
for (std::string line; lr >> line;) {
    result.push_back(std::move(line));
}
// result: {"alpha", "beta", "gamma"}
```

## See Also

- [fixed_frames](fixed_frames.md) -- split byte stream into fixed-size frames
- [byte_reader](byte_reader.md) -- produce byte chunks from a file descriptor
- [byte_writer](byte_writer.md) -- consume byte chunks to a file descriptor
