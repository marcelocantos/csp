#pragma once

#include <csp/csp.h>

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
using ssize_t = ptrdiff_t;
#endif
#else
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

// --- Platform socket type (internal raw type) ---

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t invalid_socket = -1;
#endif

// --- Opaque file descriptor wrapper ---
//
// Wraps a platform file descriptor (int on Unix, SOCKET on Windows)
// with no implicit conversion to the underlying integer type.
// All CSP functions that produce fds return fd_t already set
// non-blocking.

class fd_t {
    socket_t fd_;

public:
    constexpr fd_t() : fd_(invalid_socket) {}
    constexpr explicit fd_t(socket_t fd) : fd_(fd) {}

    // Explicit access to the raw descriptor. Use sparingly —
    // prefer the typed API.
    [[nodiscard]] constexpr socket_t raw() const { return fd_; }

    // True if this fd holds a valid descriptor.
    [[nodiscard]] constexpr bool valid() const {
        return fd_ != invalid_socket;
    }
    constexpr explicit operator bool() const { return valid(); }

    // Check whether the fd is set non-blocking (Unix only).
    // On Windows, always returns true (no kernel query available).
    [[nodiscard]] bool is_nonblock() const {
#ifdef _WIN32
        return true;  // No portable query on Windows.
#else
        int flags = ::fcntl(fd_, F_GETFL);
        return flags >= 0 && (flags & O_NONBLOCK);
#endif
    }

    friend constexpr auto operator<=>(fd_t, fd_t) = default;
    friend constexpr bool operator==(fd_t, fd_t) = default;
};

constexpr fd_t invalid_fd{};

// --- Layer 1: Suspend until fd is ready ---

inline void wait_readable(fd_t fd) { internal::io_wait_readable(fd.raw()); }
inline void wait_writable(fd_t fd) { internal::io_wait_writable(fd.raw()); }

// --- Utility ---

#ifdef _WIN32

// Set socket to non-blocking mode. Returns 0 on success, -1 on error.
inline int set_nonblock(fd_t fd) {
    u_long mode = 1;
    return ioctlsocket(fd.raw(), FIONBIO, &mode) == 0 ? 0 : -1;
}

// Close a socket.
inline void close(fd_t fd) {
    closesocket(fd.raw());
}

#else

// Set fd to non-blocking mode. Returns 0 on success, -1 on error.
int set_nonblock(fd_t fd);

// Close a file descriptor.
inline void close(fd_t fd) {
    ::close(fd.raw());
}

#endif

// --- Layer 2: Non-blocking wrappers ---
// These retry on would-block by suspending the imp until the fd
// is ready, then retrying the syscall. All retry on interrupt.

#ifdef _WIN32

// Read up to len bytes. Returns bytes read, 0 on EOF, -1 on error.
[[nodiscard]] inline ssize_t read(fd_t fd, void* buf, size_t len) {
    for (;;) {
        int n = ::recv(fd.raw(), static_cast<char*>(buf), static_cast<int>(len), 0);
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
[[nodiscard]] inline ssize_t write(fd_t fd, const void* buf, size_t len) {
    size_t written = 0;
    auto p = static_cast<const char*>(buf);
    while (written < len) {
        int n = ::send(fd.raw(), p + written, static_cast<int>(len - written), 0);
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

// Accept a connection. Returns new fd (already non-blocking), or invalid_fd on error.
[[nodiscard]] inline fd_t accept(fd_t listen_fd,
                                 struct sockaddr* addr,
                                 socklen_t* addrlen) {
    for (;;) {
        SOCKET raw = ::accept(listen_fd.raw(), addr, addrlen);
        if (raw != INVALID_SOCKET) {
            fd_t fd(raw);
            set_nonblock(fd);
            return fd;
        }
        int err = WSAGetLastError();
        if (err == WSAEINTR) continue;
        if (err == WSAEWOULDBLOCK) {
            wait_readable(listen_fd);
            continue;
        }
        return invalid_fd;
    }
}

// Non-blocking connect. Returns 0 on success, -1 on error.
// The socket must already be non-blocking.
[[nodiscard]] inline int connect(fd_t fd,
                                 const struct sockaddr* addr,
                                 socklen_t addrlen) {
    int ret = ::connect(fd.raw(), addr, addrlen);
    if (ret == 0) return 0;
    int err = WSAGetLastError();
    if (err != WSAEWOULDBLOCK) return -1;

    wait_writable(fd);
    int optval = 0;
    int optlen = sizeof(optval);
    if (getsockopt(fd.raw(), SOL_SOCKET, SO_ERROR,
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
[[nodiscard]] ssize_t read(fd_t fd, void* buf, size_t len);

// Write all of buf. Returns total bytes written, or -1 on error.
// Partial writes are retried automatically.
[[nodiscard]] ssize_t write(fd_t fd, const void* buf, size_t len);

// Accept a connection. Returns new fd (already non-blocking), or invalid_fd on error.
[[nodiscard]] fd_t accept(fd_t listen_fd, struct sockaddr* addr, socklen_t* addrlen);

// Non-blocking connect. Returns 0 on success, -1 on error.
// The fd must already be non-blocking.
[[nodiscard]] int connect(fd_t fd, const struct sockaddr* addr, socklen_t addrlen);

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
[[nodiscard]] resolve_result resolve(const std::string& host,
                              const std::string& service = {},
                              const struct addrinfo* hints = nullptr);

// --- Convenience functions ---

// Read all bytes from fd until EOF. Returns the concatenated result.
// The fd must be non-blocking.
[[nodiscard]] std::vector<uint8_t> read_all(fd_t fd, size_t chunk_size = 4096);

// Write all of data to fd. Throws csp::error on failure.
// The fd must be non-blocking.
void write_all(fd_t fd, const std::vector<uint8_t>& data);
void write_all(fd_t fd, const void* data, size_t len);

// Split fd output into newline-delimited strings. Composes
// byte_reader | split_lines. Owns the fd and closes it on EOF.
// The fd must be non-blocking.
[[nodiscard]] csp::reader<std::string> lines(fd_t fd, size_t chunk_size = 4096);

}
