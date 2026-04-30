# I/O Reference

Non-blocking I/O primitives that integrate with the imp scheduler via
a kqueue reactor. All functions live in `namespace csp::io` unless noted.

Header: `#include "csp.h"`

All I/O functions must be called from within an imp. The reactor is a
singleton kqueue event loop running on a dedicated OS thread; when an
imp calls an I/O function and the fd is not ready, the imp
suspends cooperatively and is woken by the reactor when the fd becomes ready.

---

## Table of Contents

1. [csp::io::fd_t](#cspiofd_t) — opaque file descriptor wrapper
2. [csp::io::wait_readable / csp::io::wait_writable](#cspiowait_readable-cspiowait_writable) — suspend until fd is ready
3. [csp::io::set_nonblock](#cspioset_nonblock) — set fd to non-blocking mode
4. [csp::io::read](#cspioread) — non-blocking read
5. [csp::io::write](#cspiowrite) — non-blocking write
6. [csp::io::accept](#cspioaccept) — accept a connection (returns fd_t)
7. [csp::io::connect](#cspioconnect) — non-blocking connect
8. [csp::io::resolve](#cspioresolve) — async DNS resolution
9. [csp::io::read_all](#cspioread_all) — read all bytes until EOF
10. [csp::io::write_all](#cspiowrite_all) — write all bytes
11. [csp::io::lines](#cspiolines) — read newline-delimited strings
12. [csp::file::read / csp::file::write](#cspfileread-cspfilewrite) — file I/O via blocking pool
13. [Pull-based source abstraction](#pull-based-source-abstraction) — consumer-controlled sized reads

---

## csp::io::fd_t

Opaque file descriptor wrapper. No implicit conversion to the underlying
integer type — all CSP-produced descriptors flow as `fd_t` values.

### Declaration

```cpp
class fd_t {
public:
    constexpr fd_t();                           // default: invalid
    constexpr explicit fd_t(socket_t fd);       // wrap raw descriptor

    [[nodiscard]] constexpr socket_t raw() const;  // explicit raw access
    [[nodiscard]] constexpr bool valid() const;
    constexpr explicit operator bool() const;
    [[nodiscard]] bool is_nonblock() const;     // query O_NONBLOCK flag

    friend constexpr auto operator<=>(fd_t, fd_t) = default;
    friend constexpr bool operator==(fd_t, fd_t) = default;
};

constexpr fd_t invalid_fd{};
```

### Description

All CSP functions that produce file descriptors (`io::accept`,
`net::listen`, `net::dial`) return an `fd_t` already set to non-blocking
mode. `byte_reader` and `byte_writer` accept `fd_t` and assert (not
silently fix) non-blocking status.

Use `.raw()` only when calling platform APIs directly. For all CSP
functions, pass `fd_t` directly.

### Example

```cpp
#include "csp.h"

// Wrap a raw fd (e.g. from open(2)):
csp::io::fd_t fd{::open("/dev/null", O_RDONLY | O_NONBLOCK)};
if (!fd) { /* error */ }

// Pass to CSP functions:
auto data = csp::io::read_all(fd);
```

---

## csp::io::wait_readable / csp::io::wait_writable

Suspend the current imp until a file descriptor is ready for reading
or writing.

### Signature

```cpp
void wait_readable(fd_t fd);
void wait_writable(fd_t fd);
```

### Description

These are the low-level reactor primitives. `wait_readable` registers an
`EVFILT_READ` interest with the kqueue reactor and suspends the calling
imp. When the reactor detects that `fd` has data available (or has
reached EOF or error), it wakes the imp. `wait_writable` does the same
with `EVFILT_WRITE`.

If a cancellation scope is active, the wait competes with the cancel
signal in a `prialt`: whichever fires first wins.

The higher-level wrappers (`read`, `write`, `accept`, `connect`) call these
internally. Use them directly when building custom I/O protocols on raw file
descriptors.

### Transition rules ([syntax](transition-rules.md))

```
wait_readable(fd) ─┤fd ready├──➤ return
wait_readable(fd) ─┤fd not ready├─➤ suspend; reactor wakes imp when readable
wait_readable(fd) ─┤cancel active, cancel fires first├─➤ throw canceled{}
wait_writable(fd) ─┤fd ready├──➤ return
wait_writable(fd) ─┤fd not ready├─➤ suspend; reactor wakes imp when writable
wait_writable(fd) ─┤cancel active, cancel fires first├─➤ throw canceled{}
```

### Example

```cpp
#include "csp.h"

csp::spawn([] {
    int raw = ::open("/tmp/fifo", O_RDONLY | O_NONBLOCK);
    csp::io::fd_t fd{raw};
    csp::io::wait_readable(fd);
    // fd is now ready for reading
    char buf[1024];
    ssize_t n = ::read(fd.raw(), buf, sizeof(buf));
});
csp::schedule();
```

---

## csp::io::set_nonblock

Set a file descriptor to non-blocking mode.

### Signature

```cpp
int set_nonblock(fd_t fd);
```

### Description

Calls `fcntl` to add `O_NONBLOCK` to the file descriptor's flags. Returns 0
on success, -1 on error (with `errno` set). This is a utility function — not
imp-specific — but is typically the first thing called after wrapping a
platform-created socket in an `fd_t`.

CSP-produced `fd_t` values (from `io::accept`, `net::dial`) are already
non-blocking — no need to call `set_nonblock` on them.

### Transition rules ([syntax](transition-rules.md))

```
set_nonblock(fd) ────────────➤ fd.flags |= O_NONBLOCK; return 0
set_nonblock(fd) ─┤error├───➤ return -1; errno set
```

### Example

```cpp
int raw = ::socket(AF_INET, SOCK_STREAM, 0);
csp::io::fd_t fd{raw};
csp::io::set_nonblock(fd);
```

---

## csp::io::read

Non-blocking read from a file descriptor.

### Signature

```cpp
[[nodiscard]] ssize_t read(fd_t fd, void* buf, size_t len);
```

### Description

Attempts to read up to `len` bytes from `fd` into `buf`. If the fd is not
ready (the underlying `::read` returns `EAGAIN` / `EWOULDBLOCK`), the
imp suspends via `wait_readable` until data is available, then retries.
Interrupted calls (`EINTR`) are retried automatically.

Returns the number of bytes read on success, 0 on EOF, or -1 on error (with
`errno` set). A successful call may return fewer than `len` bytes — this is
normal for non-blocking reads.

### Transition rules ([syntax](transition-rules.md))

```
read(fd, buf, len) ─┤data available├──➤ read up to len bytes; return count
read(fd, buf, len) ─┤EOF├─────────────➤ return 0
read(fd, buf, len) ─┤EAGAIN├──────────➤ suspend until fd readable; retry
read(fd, buf, len) ─┤EINTR├───────────➤ retry immediately
read(fd, buf, len) ─┤other error├─────➤ return -1; errno set
```

### Example

```cpp
#include "csp.h"

csp::spawn([](csp::io::fd_t fd) {
    char buf[4096];
    for (;;) {
        ssize_t n = csp::io::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        // process buf[0..n-1]
    }
});
csp::schedule();
```

---

## csp::io::write

Non-blocking write to a file descriptor. Writes all bytes before returning.

### Signature

```cpp
[[nodiscard]] ssize_t write(fd_t fd, const void* buf, size_t len);
```

### Description

Writes exactly `len` bytes from `buf` to `fd`. Unlike a raw `::write`, this
function handles partial writes automatically: if only some bytes are written,
it advances the buffer pointer and retries. If the fd is not ready (`EAGAIN` /
`EWOULDBLOCK`), the imp suspends via `wait_writable` until the fd can
accept data. Interrupted calls (`EINTR`) are retried automatically.

Returns `len` on success (all bytes written), or -1 on error (with `errno`
set).

### Transition rules ([syntax](transition-rules.md))

```
write(fd, buf, len) ─┤fd writable├────➤ write bytes; advance; repeat until all written
write(fd, buf, len) ─┤all written├────➤ return len
write(fd, buf, len) ─┤EAGAIN├─────────➤ suspend until fd writable; retry
write(fd, buf, len) ─┤EINTR├──────────➤ retry immediately
write(fd, buf, len) ─┤other error├────➤ return -1; errno set
```

### Example

```cpp
#include "csp.h"

csp::spawn([](csp::io::fd_t fd) {
    const char* msg = "hello, world\n";
    ssize_t n = csp::io::write(fd, msg, strlen(msg));
    // n == strlen(msg) on success, -1 on error
});
csp::schedule();
```

---

## csp::io::accept

Accept a connection on a listening socket.

### Signature

```cpp
[[nodiscard]] fd_t accept(fd_t listen_fd, struct sockaddr* addr, socklen_t* addrlen);
```

### Description

Accepts an incoming connection on `listen_fd`. If no connection is pending
(`EAGAIN` / `EWOULDBLOCK`), the imp suspends via `wait_readable` until
the reactor signals that a connection is ready. Interrupted calls (`EINTR`)
are retried automatically.

Returns a new `fd_t` already set to non-blocking mode on success, or
`invalid_fd` on error. The caller is responsible for closing the returned fd.

### Transition rules ([syntax](transition-rules.md))

```
accept(fd, addr, len) ─┤connection pending├─➤ return new fd_t (non-blocking)
accept(fd, addr, len) ─┤EAGAIN├─────────────➤ suspend until fd readable; retry
accept(fd, addr, len) ─┤EINTR├──────────────➤ retry immediately
accept(fd, addr, len) ─┤other error├────────➤ return invalid_fd; errno set
```

### Example

```cpp
#include "csp.h"
#include <netinet/in.h>

csp::spawn([] {
    int raw = socket(AF_INET6, SOCK_STREAM, 0);
    csp::io::fd_t listen_fd{raw};
    csp::io::set_nonblock(listen_fd);
    // bind + listen omitted for brevity

    for (;;) {
        csp::io::fd_t client = csp::io::accept(listen_fd, nullptr, nullptr);
        if (!client) break;

        // client is already non-blocking
        csp::spawn([client] {
            char buf[1024];
            ssize_t n = csp::io::read(client, buf, sizeof(buf));
            if (n > 0) csp::io::write(client, buf, static_cast<size_t>(n));
            csp::io::close(client);
        });
    }
    csp::io::close(listen_fd);
});
csp::schedule();
```

---

## csp::io::connect

Initiate a non-blocking TCP connection.

### Signature

```cpp
[[nodiscard]] int connect(fd_t fd, const struct sockaddr* addr, socklen_t addrlen);
```

### Description

Starts a connection to `addr` on a non-blocking socket `fd`. The fd must
already be in non-blocking mode (via `set_nonblock`). If the kernel returns
`EINPROGRESS` (the usual case for non-blocking connect), the imp
suspends via `wait_writable` until the connection attempt completes. On
resumption, `getsockopt(SO_ERROR)` is checked to determine success.

Returns 0 on success, or -1 on error (with `errno` set to the connection
error).

### Transition rules ([syntax](transition-rules.md))

```
connect(fd, addr, len) ─┤immediate success├──➤ return 0
connect(fd, addr, len) ─┤EINPROGRESS├────────➤ suspend until fd writable;
                                               check SO_ERROR; return 0 or -1
connect(fd, addr, len) ─┤other error├────────➤ return -1; errno set
```

### Example

```cpp
#include "csp.h"
#include <netinet/in.h>

csp::spawn([] {
    int raw = socket(AF_INET, SOCK_STREAM, 0);
    csp::io::fd_t fd{raw};
    csp::io::set_nonblock(fd);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(8080);

    if (csp::io::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        const char* msg = "GET / HTTP/1.0\r\n\r\n";
        csp::io::write(fd, msg, strlen(msg));
        auto data = csp::io::read_all(fd);
    }
    csp::io::close(fd);
});
csp::schedule();
```

---

## csp::io::resolve

Asynchronous DNS resolution.

### Signature

```cpp
struct resolve_result {
    addrinfo_ptr info;
    int error = 0;
    explicit operator bool() const;        // true on success
    const char* message() const;           // gai_strerror(error)
};

[[nodiscard]] resolve_result resolve(const std::string& host,
                                     const std::string& service = {},
                                     const struct addrinfo* hints = nullptr);
```

### Description

Resolves a hostname and/or service name to a list of socket addresses.
Internally, `getaddrinfo` is offloaded to the blocking thread pool via
`csp::blocking`, so the calling imp suspends cooperatively instead
of blocking its processor.

On success, `result.info` holds an `addrinfo_ptr` (a `unique_ptr` with a
custom deleter that calls `freeaddrinfo`). On failure, `result.error` holds
the `EAI_*` error code and `result.message()` provides a human-readable
description.

### Transition rules ([syntax](transition-rules.md))

```
resolve(host, svc, hints) ──────────➤ offload getaddrinfo to blocking pool;
                                      suspend calling imp;
                                      return resolve_result

bool(result)              ──────────➤ true: result.info contains addrinfo list
!bool(result)             ──────────➤ result.message() describes failure
```

### Example

```cpp
#include "csp.h"

csp::spawn([] {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    auto result = csp::io::resolve("example.com", "80", &hints);
    if (!result) {
        fprintf(stderr, "resolve failed: %s\n", result.message());
        return;
    }

    auto* ai = result.info.get();
    int raw = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    csp::io::fd_t fd{raw};
    csp::io::set_nonblock(fd);
    csp::io::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
    // ...
});
csp::schedule();
```

---

## csp::io::read_all

Read all bytes from a file descriptor until EOF.

### Signature

```cpp
[[nodiscard]] std::vector<uint8_t> read_all(fd_t fd, size_t chunk_size = 4096);
```

### Description

Calls `csp::io::read` in a loop until EOF (return value 0) or error.
Suspends cooperatively while waiting for data. Returns all bytes read as
a contiguous vector.

### Example

```cpp
#include "csp.h"

csp::spawn([](csp::io::fd_t fd) {
    auto data = csp::io::read_all(fd);
    std::string s(data.begin(), data.end());
});
csp::schedule();
```

---

## csp::io::write_all

Write all bytes to a file descriptor.

### Signature

```cpp
void write_all(fd_t fd, const std::vector<uint8_t>& data);
void write_all(fd_t fd, const void* data, size_t len);
```

### Description

Calls `csp::io::write` and throws `csp::error` if the write is
incomplete (partial write or error). Suspends cooperatively while waiting
for the fd to accept data.

### Example

```cpp
#include "csp.h"

csp::spawn([](csp::io::fd_t fd) {
    std::string msg = "hello\n";
    csp::io::write_all(fd, msg.data(), msg.size());
});
csp::schedule();
```

---

## csp::io::lines

Read newline-delimited strings from a file descriptor.

### Signature

```cpp
[[nodiscard]] csp::reader<std::string> lines(fd_t fd, size_t chunk_size = 4096);
```

### Description

Composes `byte_reader(fd) | split_lines` into a `reader<std::string>`.
Each read from the returned reader yields one newline-delimited line
(newline stripped). A partial trailing line (no trailing `\n`) is flushed
when the fd reaches EOF.

The fd must be non-blocking. `lines` owns the fd and closes it on EOF or
when the reader is dropped.

See also `part::io::lines` (identical; `csp::io::lines` forwards to it).

### Example

```cpp
#include "csp.h"

csp::spawn([](csp::io::fd_t fd) {
    for (std::string line; (csp::io::lines(fd) >> line);) {
        printf("%s\n", line.c_str());
    }
});
csp::schedule();
```

---

## csp::file::read / csp::file::write

Blocking file I/O, offloaded to the blocking thread pool.

Header: `#include "csp.h"` (via `csp/file.h`)

### Signature

```cpp
namespace csp::file {

[[nodiscard]] std::vector<uint8_t> read(const std::string& path);

void write(const std::string& path, const std::vector<uint8_t>& data);
void write(const std::string& path, const void* data, size_t len);

}
```

### Description

`file::read` reads an entire file into memory in one shot.
`file::write` creates or overwrites a file atomically (truncates on open).
Both offload the blocking OS calls to the blocking thread pool via
`csp::blocking`, so the calling imp suspends cooperatively.

Both throw `csp::error` on failure (file not found, permission denied, etc.).

These functions use `std::ifstream` / `std::ofstream` internally and are
suitable for configuration files, small data files, and test fixtures.
For large streaming files or fine-grained I/O, use `io::read` / `io::write`
with an `fd_t` opened with `O_NONBLOCK`.

### Example

```cpp
#include "csp.h"

csp::spawn([] {
    // Write then read a file.
    std::string msg = "hello, file I/O";
    std::vector<uint8_t> data(msg.begin(), msg.end());
    csp::file::write("/tmp/demo.txt", data);

    auto result = csp::file::read("/tmp/demo.txt");
    std::string s(result.begin(), result.end());
    assert(s == msg);
});
csp::schedule();
```

---

## Pull-based source abstraction

Header: `#include "csp/source.h"` (included by `"csp.h"`)

A **source** inverts the push model: instead of the producer choosing
chunk sizes, the consumer tells the source how many bytes it wants.  This
is the CSP-native pull pattern built from `request<Req, Resp>`.

### Type aliases

```cpp
namespace csp::io {

// A single pull request: ask for up to `value` bytes; the source writes
// up to that many into the `reply` one-shot channel.
using read_request = request<size_t, bytes>;

// The consumer-facing handle.  A source is the *write* end of a request
// channel.  Consumers write requests into it and read replies from the
// per-request reply channels.
using source = writer<read_request>;

} // namespace csp::io
```

### Outcome channels

Three structurally distinct paths carry the three possible outcomes:

| Outcome | Mechanism |
|---------|-----------|
| **Success** | The reply channel carries a `bytes` value (`<= n` bytes). |
| **EOF** | The source imp exits; the reply-writer drops; `reply >> buf` returns `false`. |
| **Error** | The source calls `req.reply._throw(ep)` before exiting; `reply >> buf` rethrows. |

### fd_source

```cpp
[[nodiscard]] csp::io::source fd_source(csp::io::fd_t fd);
```

Spawns an imp that serves `read_request` values from a non-blocking fd.
Partial reads (fewer bytes than requested) are normal and match `read(2)`
semantics.  The imp owns the fd and closes it on exit.

**Zero-byte requests** throw `std::invalid_argument` across the reply
and exit — this is almost always a caller bug.

### errno_error

```cpp
class csp::io::errno_error : public csp::error {
public:
    errno_error(const std::string& syscall, int err);
    int err() const;
};
```

Thrown (via `_throw`) when `io::read` returns a negative value.  Carries
the syscall name and the `errno` at the time of failure.

### Convenience helpers

```cpp
// Blocking call: send request, wait for reply, return bytes.
// Throws errno_error on I/O error; throws channel_closed on EOF.
bytes source_read(source& s, size_t n);

// Non-blocking call: returns a reader<bytes> for use in prialt.
reader<bytes> call_source(source& s, size_t n);
```

### Examples

**Sequential reads:**

```cpp
#include "csp.h"

auto s = csp::io::fd_source(rfd);

csp::spawn([s = std::move(s)]() mutable {
    for (;;) {
        auto reply = csp::io::call_source(s, 4096);
        csp::bytes chunk;
        if (!(reply >> chunk)) break;  // EOF
        // process chunk ...
    }
});
```

**prialt across two sources:**

```cpp
auto r1 = csp::io::call_source(s1, 4096);
auto r2 = csp::io::call_source(s2, 4096);
csp::bytes b1, b2;

switch (csp::prialt(r1 >> b1, r2 >> b2)) {
case 0: /* b1 ready */ break;
case 1: /* b2 ready */ break;
}
```

**writer::operator() sugar (blocking):**

```cpp
csp::bytes chunk = s(4096);  // blocks until reply; throws on EOF/error
```

### See also

- Design paper: `docs/papers/19-pull-based-sources.md`
- `request<Req, Resp>` and `call()` in [Channels reference](channels.md)
- [🎯T17](../../bullseye.yaml) — full pull-based source convergence target
