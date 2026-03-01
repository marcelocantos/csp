#pragma once

#include <csp/blocking.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
using ssize_t = ptrdiff_t;
#endif
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace csp::internal {

#ifdef _WIN32
void io_wait_readable(SOCKET sock);
void io_wait_writable(SOCKET sock);
#else
// Layer 1 primitives — defined in src/io.cc.
// Cancel-aware: if a cancel guard is active, these compose the
// fd readiness signal with the cancel signal in a prialt.
void io_wait_readable(int fd);
void io_wait_writable(int fd);
#endif

}

namespace csp::io {

// --- Platform socket type ---

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t invalid_socket = -1;
#endif

// --- Layer 1: Suspend until fd is ready ---

inline void wait_readable(socket_t fd) { internal::io_wait_readable(fd); }
inline void wait_writable(socket_t fd) { internal::io_wait_writable(fd); }

// --- Utility ---

#ifdef _WIN32

// Set socket to non-blocking mode. Returns 0 on success, -1 on error.
inline int set_nonblock(socket_t fd) {
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
}

// Close a socket.
inline void close(socket_t fd) {
    closesocket(fd);
}

#else

// Set fd to non-blocking mode. Returns 0 on success, -1 on error.
inline int set_nonblock(socket_t fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Close a file descriptor.
inline void close(socket_t fd) {
    ::close(fd);
}

#endif

// --- Layer 2: Non-blocking wrappers ---
// These retry on would-block by suspending the imp until the fd
// is ready, then retrying the syscall. All retry on interrupt.

#ifdef _WIN32

// Read up to len bytes. Returns bytes read, 0 on EOF, -1 on error.
[[nodiscard]] inline ssize_t read(socket_t fd, void* buf, size_t len) {
    for (;;) {
        int n = ::recv(fd, static_cast<char*>(buf), static_cast<int>(len), 0);
        if (n >= 0) return n;
        int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        if (err == WSAEWOULDBLOCK) {
            wait_readable(fd);
            continue;
        }
        return -1;
    }
}

// Write all of buf. Returns total bytes written, or -1 on error.
// Partial writes are retried automatically.
[[nodiscard]] inline ssize_t write(socket_t fd, const void* buf, size_t len) {
    size_t written = 0;
    auto p = static_cast<const char*>(buf);
    while (written < len) {
        int n = ::send(fd, p + written, static_cast<int>(len - written), 0);
        if (n >= 0) {
            written += static_cast<size_t>(n);
            continue;
        }
        int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        if (err == WSAEWOULDBLOCK) {
            wait_writable(fd);
            continue;
        }
        return -1;
    }
    return static_cast<ssize_t>(written);
}

// Accept a connection. Returns new socket, or INVALID_SOCKET on error.
[[nodiscard]] inline socket_t accept(socket_t listen_fd,
                                     struct sockaddr* addr,
                                     socklen_t* addrlen) {
    for (;;) {
        SOCKET fd = ::accept(listen_fd, addr, addrlen);
        if (fd != INVALID_SOCKET) return fd;
        int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        if (err == WSAEWOULDBLOCK) {
            wait_readable(listen_fd);
            continue;
        }
        return INVALID_SOCKET;
    }
}

// Non-blocking connect. Returns 0 on success, -1 on error.
// The socket must already be non-blocking.
[[nodiscard]] inline int connect(socket_t fd,
                                 const struct sockaddr* addr,
                                 socklen_t addrlen) {
    int ret = ::connect(fd, addr, addrlen);
    if (ret == 0) return 0;
    int err = WSAGetLastError();
    if (err != WSAEWOULDBLOCK) return -1;

    wait_writable(fd);
    int optval = 0;
    int optlen = sizeof(optval);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&optval), &optlen) < 0)
        return -1;
    if (optval != 0) {
        WSASetLastError(optval);
        return -1;
    }
    return 0;
}

#else // !_WIN32

// Read up to len bytes. Returns bytes read, 0 on EOF, -1 on error.
[[nodiscard]] inline ssize_t read(socket_t fd, void* buf, size_t len) {
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
[[nodiscard]] inline ssize_t write(socket_t fd, const void* buf, size_t len) {
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
[[nodiscard]] inline socket_t accept(socket_t listen_fd,
                                     struct sockaddr* addr,
                                     socklen_t* addrlen) {
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
[[nodiscard]] inline int connect(socket_t fd,
                                 const struct sockaddr* addr,
                                 socklen_t addrlen) {
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

#endif // _WIN32

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

    const char* message() const {
#ifdef _WIN32
        // gai_strerror is not thread-safe on Windows.
        // Use gai_strerrorA which is the narrow-char version.
        return gai_strerrorA(error);
#else
        return gai_strerror(error);
#endif
    }
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
