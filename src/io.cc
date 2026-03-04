#include <csp/blocking.h>
#include <csp/cancel.h>
#include <csp/io.h>
#include <csp/internal/signal.h>
#include <csp/internal/reactor.h>

#include <cerrno>
#include <cstdint>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace csp::internal {

#ifdef _WIN32

void io_wait_readable(SOCKET sock) {
    auto signal = detail::create_fd_readable(sock);

    if (!csp::is_cancel_active()) {
        csp::prialt(~signal);
        return;
    }

    switch (csp::prialt(csp::done(), ~signal)) {
    case ~0: {
        auto reason = csp::cancel_reason();
        if (reason) std::rethrow_exception(reason);
        throw csp::canceled{};
    }
    case ~1: return;
    }
}

void io_wait_writable(SOCKET sock) {
    auto signal = detail::create_fd_writable(sock);

    if (!csp::is_cancel_active()) {
        csp::prialt(~signal);
        return;
    }

    switch (csp::prialt(csp::done(), ~signal)) {
    case ~0: {
        auto reason = csp::cancel_reason();
        if (reason) std::rethrow_exception(reason);
        throw csp::canceled{};
    }
    case ~1: return;
    }
}

#else // !_WIN32

void io_wait_readable(int fd) {
    auto signal = detail::create_fd_readable(fd);

    if (!csp::is_cancel_active()) {
        csp::prialt(~signal);
        return;
    }

    switch (csp::prialt(csp::done(), ~signal)) {
    case ~0: {
        auto reason = csp::cancel_reason();
        if (reason) std::rethrow_exception(reason);
        throw csp::canceled{};
    }
    case ~1: return;
    }
}

void io_wait_writable(int fd) {
    auto signal = detail::create_fd_writable(fd);

    if (!csp::is_cancel_active()) {
        csp::prialt(~signal);
        return;
    }

    switch (csp::prialt(csp::done(), ~signal)) {
    case ~0: {
        auto reason = csp::cancel_reason();
        if (reason) std::rethrow_exception(reason);
        throw csp::canceled{};
    }
    case ~1: return;
    }
}

#endif // _WIN32

} // namespace csp::internal

namespace csp::io {

#ifndef _WIN32

int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

ssize_t read(int fd, void* buf, size_t len) {
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

ssize_t write(int fd, const void* buf, size_t len) {
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

int accept(int listen_fd, struct sockaddr* addr, socklen_t* addrlen) {
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

int connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
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

#endif // !_WIN32

resolve_result resolve(const std::string& host,
                       const std::string& service,
                       const struct addrinfo* hints) {
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

} // namespace csp::io
