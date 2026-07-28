#include <csp/blocking.h>
#include <csp/cancel.h>
#include <csp/io.h>
#include <csp/part/io.h>
#include <csp/internal/signal.h>
#include <csp/internal/reactor.h>

#include <cerrno>
#include <cstdint>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace csp::internal {

namespace {

// Cancel-aware wait on an fd readiness signal (🎯T48: one body for both
// directions on both platforms — io::socket_t abstracts SOCKET vs int).
void wait_signal(detail::fd_signal signal) {
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

} // anonymous namespace

void io_wait_readable(io::socket_t sock) {
    wait_signal(detail::create_fd_readable(sock));
}

void io_wait_writable(io::socket_t sock) {
    wait_signal(detail::create_fd_writable(sock));
}

} // namespace csp::internal

namespace csp::io {

#ifndef _WIN32

int set_nonblock(fd_t fd) {
    int flags = fcntl(fd.raw(), F_GETFL);
    if (flags < 0) return -1;
    return fcntl(fd.raw(), F_SETFL, flags | O_NONBLOCK);
}

ssize_t read(fd_t fd, void* buf, size_t len) {
    for (;;) {
        ssize_t n = ::read(fd.raw(), buf, len);
        if (n >= 0) return n;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_readable(fd);
            continue;
        }
        return -1;
    }
}

ssize_t write(fd_t fd, const void* buf, size_t len) {
    size_t written = 0;
    auto p = static_cast<const uint8_t*>(buf);
    while (written < len) {
        ssize_t n = ::write(fd.raw(), p + written, len - written);
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

fd_t accept(fd_t listen_fd, struct sockaddr* addr, socklen_t* addrlen) {
    for (;;) {
        int raw = ::accept(listen_fd.raw(), addr, addrlen);
        if (raw >= 0) {
            fd_t fd(raw);
            set_nonblock(fd);
            return fd;
        }
        int err = errno;
        if (err == EINTR) continue;
        if (err == EAGAIN || err == EWOULDBLOCK) {
            wait_readable(listen_fd);
            continue;
        }
        return invalid_fd;
    }
}

int connect(fd_t fd, const struct sockaddr* addr, socklen_t addrlen) {
    int ret = ::connect(fd.raw(), addr, addrlen);
    if (ret == 0) return 0;
    if (errno != EINPROGRESS) return -1;

    wait_writable(fd);
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(fd.raw(), SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) return -1;
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

std::string format_addr(const struct sockaddr* sa, socklen_t len) {
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    int rc = getnameinfo(sa, len, host, sizeof(host), serv, sizeof(serv),
                         NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) return "unknown";
    if (sa->sa_family == AF_INET6)
        return std::string("[") + host + "]:" + serv;
    return std::string(host) + ":" + serv;
}

std::vector<uint8_t> read_all(fd_t fd, size_t chunk_size) {
    std::vector<uint8_t> result;
    std::vector<uint8_t> buf(chunk_size);
    for (;;) {
        ssize_t n = read(fd, buf.data(), buf.size());
        if (n <= 0) break;
        result.insert(result.end(), buf.data(), buf.data() + n);
    }
    return result;
}

void write_all(fd_t fd, const std::vector<uint8_t>& data) {
    write_all(fd, data.data(), data.size());
}

void write_all(fd_t fd, const void* data, size_t len) {
    ssize_t n = write(fd, data, len);
    if (n < 0 || static_cast<size_t>(n) != len) {
        throw csp::error("write_all: incomplete write");
    }
}

csp::reader<std::string> lines(fd_t fd, size_t chunk_size) {
    return csp::part::io::lines(fd, chunk_size);
}

} // namespace csp::io
