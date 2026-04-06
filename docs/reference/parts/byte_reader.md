# csp::part::io::byte_reader

Produces byte chunks from a non-blocking file descriptor. Each message contains
as much data as was available from a single `read()` call. Owns the fd and
closes it on exit.

## Signature

```cpp
auto byte_reader(io::fd_t fd, size_t chunk_size = 4096);
// Returns: producer<std::vector<uint8_t>, ...>
```

## Topology

<!-- csp-flow
fd -> {byte_reader(fd)} -> reader<vector<uint8_t>>
-->
![byte_reader topology](diagrams/byte_reader.svg)

One internal imp reads from the fd in a loop. Each successful `read()`
produces one channel message containing the bytes that were available.

## Semantics

- **fd ownership**: `byte_reader` takes ownership of `fd` (an `io::fd_t`).
  It closes the fd when the imp exits. The caller must not read from or close
  the fd after passing it.
- **Non-blocking required**: The fd must already be in non-blocking mode
  (`O_NONBLOCK`). `byte_reader` asserts this at startup; it does not call
  `set_nonblock` itself. Use `io::accept` (which returns a non-blocking `fd_t`)
  or call `io::set_nonblock` before passing the fd.
- **Non-blocking I/O**: Uses `csp::io::read()`, which suspends the imp
  on `EAGAIN`/`EWOULDBLOCK` (via `io::wait_readable`) and retries on `EINTR`.
  The processor is never blocked.
- **Chunk sizing**: The `chunk_size` parameter controls the read buffer size.
  Each message may contain fewer bytes than `chunk_size` (whatever the kernel
  returned). The default is 4096 bytes.
- **EOF**: When `read()` returns 0, the imp closes the fd and exits,
  closing the output channel. Downstream readers see channel death.
- **Backpressure**: When the downstream consumer is slow, the channel write
  (`out << ...`) blocks the imp. During this time, data accumulates
  in kernel buffers. If kernel buffers fill, the writing end of the pipe or
  socket will also block (or signal `EAGAIN` to its writer).
- **Error handling**: Any read error other than `EAGAIN`/`EINTR` causes the
  imp to close the fd and exit.

## Example

```cpp
#include "csp.h"

using namespace csp;
using namespace csp::part;

int raw[2];
pipe(raw);
io::fd_t rfd{raw[0]}, wfd{raw[1]};
io::set_nonblock(rfd);

// byte_reader owns rfd and closes it on exit.
auto r = io::byte_reader(std::move(rfd), 16).spawn();

csp::spawn([wfd = std::move(wfd)] {
    const char* msg = "Hello, CSP!";
    ::write(wfd.raw(), msg, strlen(msg));
    // wfd closes automatically when it goes out of scope
});

std::vector<uint8_t> all;
for (std::vector<uint8_t> chunk; r >> chunk;) {
    all.insert(all.end(), chunk.begin(), chunk.end());
}
// all contains "Hello, CSP!" as bytes
```

## See Also

- [byte_writer](byte_writer.md) -- write byte chunks to an fd
- [split_lines](split_lines.md) -- split byte stream into newline-delimited strings
- [fixed_frames](fixed_frames.md) -- split byte stream into fixed-size frames
