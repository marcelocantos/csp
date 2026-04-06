# csp::part::io::byte_writer

Consumes byte chunks from a channel and writes them to a file descriptor. Owns
the fd and closes it on exit.

## Signature

```cpp
auto byte_writer(io::fd_t fd);
// Returns: consumer<std::vector<uint8_t>, ...>
```

## Topology

<!-- csp-flow
writer<vector<uint8_t>> -> {byte_writer(fd)} -> fd
-->
![byte_writer topology](diagrams/byte_writer.svg)

One internal imp reads byte chunks from the input channel and writes
each one to the fd using `csp::io::write()`.

## Semantics

- **fd ownership**: `byte_writer` takes ownership of `fd` (an `io::fd_t`).
  It closes the fd when the imp exits. The caller must not write to or close
  the fd after passing it.
- **Non-blocking required**: The fd must already be in non-blocking mode
  (`O_NONBLOCK`). `byte_writer` asserts this at startup; it does not call
  `set_nonblock` itself. Use `io::accept` (which returns a non-blocking `fd_t`)
  or call `io::set_nonblock` before passing the fd.
- **Non-blocking I/O**: Uses `csp::io::write()`, which handles partial writes
  automatically and suspends the imp on `EAGAIN`/`EWOULDBLOCK` (via
  `io::wait_writable`). Retries on `EINTR`. The processor is never blocked.
- **Complete writes**: `io::write()` loops until all bytes in the chunk are
  written. A single channel message is always written atomically to the fd
  (modulo kernel-level interleaving with other writers).
- **Input close**: When the upstream writer is dropped, the input channel
  dies. The imp exits its read loop, closes the fd, and terminates.
- **Backpressure**: When the kernel buffer is full, the imp suspends
  on `io::wait_writable`. This blocks the channel read, which in turn applies
  backpressure to upstream producers.
- **Error handling**: Any write error other than `EAGAIN`/`EINTR` causes the
  imp to close the fd and exit.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

int raw[2];
pipe(raw);
io::fd_t rfd{raw[0]}, wfd{raw[1]};
io::set_nonblock(wfd);

// byte_writer owns wfd and closes it on exit.
auto w = io::byte_writer(std::move(wfd)).spawn();

csp::spawn([w = std::move(w)] {
    std::string msg = "CSP writes!";
    std::vector<uint8_t> chunk(msg.begin(), msg.end());
    w << std::move(chunk);
});

// Read from rfd to receive "CSP writes!"
```

## See Also

- [byte_reader](byte_reader.md) -- read byte chunks from an fd
- [split_lines](split_lines.md) -- split byte stream into newline-delimited strings
- [fixed_frames](fixed_frames.md) -- split byte stream into fixed-size frames
