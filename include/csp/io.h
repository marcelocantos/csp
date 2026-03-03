#pragma once

#include <cstddef>
#include <netdb.h>
#include <sys/socket.h>

#include <memory>
#include <string>

namespace csp::internal {

// Layer 1 primitives — defined in src/io.cc.
// Cancel-aware: if a cancel guard is active, these compose the
// fd readiness signal with the cancel signal in a prialt.
void io_wait_readable(int fd);
void io_wait_writable(int fd);

}

namespace csp::io {

// --- Layer 1: Suspend until fd is ready ---

inline void wait_readable(int fd) { internal::io_wait_readable(fd); }
inline void wait_writable(int fd) { internal::io_wait_writable(fd); }

// --- Utility ---

// Set fd to non-blocking mode. Returns 0 on success, -1 on error.
int set_nonblock(int fd);

// --- Layer 2: Non-blocking wrappers ---
// These retry on EAGAIN by suspending the imp until the fd
// is ready, then retrying the syscall. All retry on EINTR.

// Read up to len bytes. Returns bytes read, 0 on EOF, -1 on error.
[[nodiscard]] ssize_t read(int fd, void* buf, size_t len);

// Write all of buf. Returns total bytes written, or -1 on error.
// Partial writes are retried automatically.
[[nodiscard]] ssize_t write(int fd, const void* buf, size_t len);

// Accept a connection. Returns new fd, or -1 on error.
[[nodiscard]] int accept(int listen_fd, struct sockaddr* addr, socklen_t* addrlen);

// Non-blocking connect. Returns 0 on success, -1 on error.
// The fd must already be non-blocking.
[[nodiscard]] int connect(int fd, const struct sockaddr* addr, socklen_t addrlen);

// --- DNS resolution ---
// Offloads getaddrinfo to the blocking thread pool so the calling
// imp suspends cooperatively instead of blocking its processor.

struct addrinfo_deleter {
    void operator()(struct addrinfo* p) const { if (p) freeaddrinfo(p); }
};
using addrinfo_ptr = std::unique_ptr<struct addrinfo, addrinfo_deleter>;

struct resolve_result {
    addrinfo_ptr info;
    int error = 0;
    explicit operator bool() const { return error == 0; }
    const char* message() const { return gai_strerror(error); }
};

// Resolve host/service. hints may be nullptr for defaults.
// Runs getaddrinfo on the blocking pool — never stalls the processor.
[[nodiscard]] resolve_result resolve(const std::string& host,
                              const std::string& service = {},
                              const struct addrinfo* hints = nullptr);

}
