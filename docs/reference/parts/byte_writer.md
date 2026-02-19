# byte_writer

Consumes byte chunks from a channel and writes them to a file descriptor. Owns
the fd and closes it on exit.

## Signature

```cpp
auto byte_writer(int fd);
// Returns: consumer<std::vector<uint8_t>, ...>
```

## Topology

```mermaid
graph LR
    W["writer&lt;vector&lt;uint8_t&gt;&gt;"] --> BW["byte_writer(fd)"] --> FD["fd (pipe, socket, ...)"]
```

One internal microthread reads byte chunks from the input channel and writes
each one to the fd using `csp::io::write()`.

## Semantics

- **fd ownership**: `byte_writer` takes ownership of `fd`. It sets the fd to
  non-blocking mode (`O_NONBLOCK`) and closes it when the microthread exits.
  The caller must not write to or close the fd after passing it.
- **Non-blocking I/O**: Uses `csp::io::write()`, which handles partial writes
  automatically and suspends the microthread on `EAGAIN`/`EWOULDBLOCK` (via
  `io::wait_writable`). Retries on `EINTR`. The processor is never blocked.
- **Complete writes**: `io::write()` loops until all bytes in the chunk are
  written. A single channel message is always written atomically to the fd
  (modulo kernel-level interleaving with other writers).
- **Input close**: When the upstream writer is dropped, the input channel
  dies. The microthread exits its read loop, closes the fd, and terminates.
- **Backpressure**: When the kernel buffer is full, the microthread suspends
  on `io::wait_writable`. This blocks the channel read, which in turn applies
  backpressure to upstream producers.
- **Error handling**: Any write error other than `EAGAIN`/`EINTR` causes the
  microthread to close the fd and exit.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

int pipefd[2];
pipe(pipefd);

// byte_writer owns pipefd[1] and closes it on exit.
auto w = byte_writer(pipefd[1]).spawn();

csp::spawn([w = std::move(w)] {
    std::string msg = "CSP writes!";
    std::vector<uint8_t> chunk(msg.begin(), msg.end());
    w << std::move(chunk);
});

// Read from pipefd[0] to receive "CSP writes!"
```

## See Also

- [byte_reader](byte_reader.md) -- read byte chunks from an fd
- [split_lines](split_lines.md) -- split byte stream into newline-delimited strings
- [fixed_frames](fixed_frames.md) -- split byte stream into fixed-size frames
