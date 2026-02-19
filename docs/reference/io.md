# I/O Reference

Non-blocking I/O primitives that integrate with the microthread scheduler via
a kqueue reactor. All functions live in `namespace csp::io`.

Header: `#include "csp/io.h"` (or `#include "csp.h"`)

All I/O functions must be called from within a microthread. The reactor is a
singleton kqueue event loop running on a dedicated OS thread; when a
microthread calls an I/O function and the fd is not ready, the microthread
suspends cooperatively and is woken by the reactor when the fd becomes ready.

---

## Table of Contents

1. [wait_readable / wait_writable](#wait_readable--wait_writable) -- suspend until fd is ready
2. [set_nonblock](#set_nonblock) -- set fd to non-blocking mode
3. [read](#read) -- non-blocking read
4. [write](#write) -- non-blocking write
5. [accept](#accept) -- accept a connection
6. [connect](#connect) -- non-blocking connect
7. [resolve](#resolve) -- async DNS resolution

---

## wait_readable / wait_writable

Suspend the current microthread until a file descriptor is ready for reading
or writing.

### Signature

```cpp
void wait_readable(int fd);
void wait_writable(int fd);
```

### Description

These are the low-level reactor primitives. `wait_readable` registers an
`EVFILT_READ` interest with the kqueue reactor and suspends the calling
microthread. When the reactor detects that `fd` has data available (or has
reached EOF or error), it wakes the microthread. `wait_writable` does the same
with `EVFILT_WRITE`.

The higher-level wrappers (`read`, `write`, `accept`, `connect`) call these
internally. Use them directly when building custom I/O protocols on raw file
descriptors.

### Transition rules

```
wait_readable(fd) ─┤fd ready├──➤ return
wait_readable(fd) ─┤fd not ready├─➤ suspend; reactor wakes microthread when readable
wait_writable(fd) ─┤fd ready├──➤ return
wait_writable(fd) ─┤fd not ready├─➤ suspend; reactor wakes microthread when writable
```

### Example

```cpp
#include "csp.h"

csp::spawn([] {
    int fd = /* ... open a non-blocking fd ... */;
    csp::io::wait_readable(fd);
    // fd is now ready for reading
    char buf[1024];
    ssize_t n = ::read(fd, buf, sizeof(buf));
});
csp::schedule();
```

---

## set_nonblock

Set a file descriptor to non-blocking mode.

### Signature

```cpp
int set_nonblock(int fd);
```

### Description

Calls `fcntl` to add `O_NONBLOCK` to the file descriptor's flags. Returns 0
on success, -1 on error (with `errno` set). This is a utility function -- not
microthread-specific -- but is typically the first thing called after creating
a socket that will be used with the `csp::io` wrappers.

### Transition rules

```
set_nonblock(fd) ────────────➤ fd.flags |= O_NONBLOCK; return 0
set_nonblock(fd) ─┤error├───➤ return -1; errno set
```

### Example

```cpp
int sock = socket(AF_INET, SOCK_STREAM, 0);
csp::io::set_nonblock(sock);
```

---

## read

Non-blocking read from a file descriptor.

### Signature

```cpp
ssize_t read(int fd, void* buf, size_t len);
```

### Description

Attempts to read up to `len` bytes from `fd` into `buf`. If the fd is not
ready (the underlying `::read` returns `EAGAIN` / `EWOULDBLOCK`), the
microthread suspends via `wait_readable` until data is available, then retries.
Interrupted calls (`EINTR`) are retried automatically.

Returns the number of bytes read on success, 0 on EOF, or -1 on error (with
`errno` set). A successful call may return fewer than `len` bytes -- this is
normal for non-blocking reads.

### Transition rules

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

csp::spawn([] {
    auto [w, r] = csp::chan<std::string>{};

    csp::spawn([w = std::move(w)](int fd) {
        char buf[4096];
        for (;;) {
            ssize_t n = csp::io::read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            w << std::string(buf, static_cast<size_t>(n));
        }
    });
});
csp::schedule();
```

---

## write

Non-blocking write to a file descriptor. Writes all bytes before returning.

### Signature

```cpp
ssize_t write(int fd, const void* buf, size_t len);
```

### Description

Writes exactly `len` bytes from `buf` to `fd`. Unlike a raw `::write`, this
function handles partial writes automatically: if only some bytes are written,
it advances the buffer pointer and retries. If the fd is not ready (`EAGAIN` /
`EWOULDBLOCK`), the microthread suspends via `wait_writable` until the fd can
accept data. Interrupted calls (`EINTR`) are retried automatically.

Returns `len` on success (all bytes written), or -1 on error (with `errno`
set).

### Transition rules

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

csp::spawn([] {
    int fd = /* ... connected socket ... */;
    const char* msg = "hello, world\n";
    ssize_t n = csp::io::write(fd, msg, strlen(msg));
    // n == strlen(msg) on success, -1 on error
});
csp::schedule();
```

---

## accept

Accept a connection on a listening socket.

### Signature

```cpp
int accept(int listen_fd, struct sockaddr* addr, socklen_t* addrlen);
```

### Description

Accepts an incoming connection on `listen_fd`. If no connection is pending
(`EAGAIN` / `EWOULDBLOCK`), the microthread suspends via `wait_readable` until
the reactor signals that a connection is ready. Interrupted calls (`EINTR`)
are retried automatically.

Returns the new socket fd on success, or -1 on error (with `errno` set). The
caller is responsible for setting the returned fd to non-blocking mode and
closing it when done.

### Transition rules

```
accept(fd, addr, len) ─┤connection pending├─➤ return new_fd
accept(fd, addr, len) ─┤EAGAIN├─────────────➤ suspend until fd readable; retry
accept(fd, addr, len) ─┤EINTR├──────────────➤ retry immediately
accept(fd, addr, len) ─┤other error├────────➤ return -1; errno set
```

### Example

```cpp
#include "csp.h"
#include <netinet/in.h>

csp::spawn([] {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    csp::io::set_nonblock(listen_fd);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);

    // Accept loop -- each connection handled in its own microthread
    for (;;) {
        int client_fd = csp::io::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) break;
        csp::io::set_nonblock(client_fd);

        csp::spawn([client_fd] {
            char buf[1024];
            ssize_t n = csp::io::read(client_fd, buf, sizeof(buf));
            if (n > 0) csp::io::write(client_fd, buf, static_cast<size_t>(n));
            close(client_fd);
        });
    }
    close(listen_fd);
});
csp::schedule();
```

---

## connect

Initiate a non-blocking TCP connection.

### Signature

```cpp
int connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
```

### Description

Starts a connection to `addr` on a non-blocking socket `fd`. The fd must
already be in non-blocking mode (via `set_nonblock`). If the kernel returns
`EINPROGRESS` (the usual case for non-blocking connect), the microthread
suspends via `wait_writable` until the connection attempt completes. On
resumption, `getsockopt(SO_ERROR)` is checked to determine whether the
connection succeeded or failed.

Returns 0 on success, or -1 on error (with `errno` set to the connection
error).

### Transition rules

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
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    csp::io::set_nonblock(fd);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(8080);

    if (csp::io::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        const char* msg = "GET / HTTP/1.0\r\n\r\n";
        csp::io::write(fd, msg, strlen(msg));

        char buf[4096];
        ssize_t n = csp::io::read(fd, buf, sizeof(buf));
        // process response...
    }
    close(fd);
});
csp::schedule();
```

---

## resolve

Asynchronous DNS resolution.

### Signature

```cpp
struct resolve_result {
    int error;                             // 0 on success, EAI_* on failure
    addrinfo_ptr info;                     // unique_ptr to addrinfo linked list
    const char* error_string() const;      // gai_strerror(error)
};

resolve_result resolve(const std::string& host,
                       const std::string& service = {},
                       const struct addrinfo* hints = nullptr);
```

### Description

Resolves a hostname and/or service name to a list of socket addresses.
Internally, `getaddrinfo` is offloaded to the blocking thread pool via
`csp::blocking`, so the calling microthread suspends cooperatively instead
of blocking its processor. This is important because `getaddrinfo` is a
blocking system call that can take an unpredictable amount of time (DNS
lookups, `/etc/hosts` parsing, mDNS).

On success (`error == 0`), `info` points to a linked list of `addrinfo`
results, managed by a `unique_ptr` with a custom deleter that calls
`freeaddrinfo`. On failure, `error` contains an `EAI_*` error code and
`error_string()` returns a human-readable description.

The optional `hints` parameter controls the resolution behavior (address
family, socket type, protocol, flags) -- same semantics as the `hints`
argument to `getaddrinfo(3)`.

### Transition rules

```
resolve(host, svc, hints) ──────────➤ offload getaddrinfo to blocking pool;
                                      suspend calling microthread;
                                      return resolve_result

resolve_result.error == 0 ──────────➤ info contains addrinfo linked list
resolve_result.error != 0 ──────────➤ info is null; error_string() describes failure
```

### Example

```cpp
#include "csp.h"
#include <netinet/in.h>

csp::spawn([] {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    auto result = csp::io::resolve("example.com", "80", &hints);
    if (result.error != 0) {
        // result.error_string() for details
        return;
    }

    // Connect using the first result
    int fd = socket(result.info->ai_family,
                    result.info->ai_socktype,
                    result.info->ai_protocol);
    csp::io::set_nonblock(fd);

    if (csp::io::connect(fd, result.info->ai_addr,
                         result.info->ai_addrlen) == 0) {
        // connected successfully
    }
    close(fd);
});
csp::schedule();
```
