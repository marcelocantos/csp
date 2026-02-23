#pragma once

#include <csp/blocking.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

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
inline int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// --- Layer 2: Non-blocking wrappers ---
// These retry on EAGAIN by suspending the imp until the fd
// is ready, then retrying the syscall. All retry on EINTR.

// Read up to len bytes. Returns bytes read, 0 on EOF, -1 on error.
[[nodiscard]] inline ssize_t read(int fd, void* buf, size_t len) {
    for (;;) {
        ssize_t n = ::read(fd, buf, len);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_readable(fd);
            continue;
        }
        return -1;
    }
}

// Write all of buf. Returns total bytes written, or -1 on error.
// Partial writes are retried automatically.
[[nodiscard]] inline ssize_t write(int fd, const void* buf, size_t len) {
    size_t written = 0;
    auto p = static_cast<const uint8_t*>(buf);
    while (written < len) {
        ssize_t n = ::write(fd, p + written, len - written);
        if (n >= 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_writable(fd);
            continue;
        }
        return -1;
    }
    return static_cast<ssize_t>(written);
}

// Accept a connection. Returns new fd, or -1 on error.
[[nodiscard]] inline int accept(int listen_fd, struct sockaddr* addr, socklen_t* addrlen) {
    for (;;) {
        int fd = ::accept(listen_fd, addr, addrlen);
        if (fd >= 0) return fd;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_readable(listen_fd);
            continue;
        }
        return -1;
    }
}

// Non-blocking connect. Returns 0 on success, -1 on error.
// The fd must already be non-blocking.
[[nodiscard]] inline int connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    int ret = ::connect(fd, addr, addrlen);
    if (ret == 0) return 0;
    if (errno != EINPROGRESS) return -1;

    wait_writable(fd);
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) return -1;
    if (err != 0) { errno = err; return -1; }
    return 0;
}

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
[[nodiscard]] inline resolve_result resolve(const std::string& host,
                              const std::string& service = {},
                              const struct addrinfo* hints = nullptr) {
    struct addrinfo* raw = nullptr;
    int err = csp::blocking([&] {
        return ::getaddrinfo(
            host.c_str(),
            service.empty() ? nullptr : service.c_str(),
            hints, &raw);
    });
    if (err != 0) return resolve_result{.error = err};
    return resolve_result{.info = addrinfo_ptr(raw)};
}

}
