#include <csp/net.h>
#include <csp/cancel.h>
#include <csp/internal/signal.h>

#include <cstring>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <ws2tcpip.h>
#endif

namespace csp::net {

namespace {

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

connection make_connection(io::socket_t fd, std::string remote) {
    io::set_nonblock(fd);

#ifdef _WIN32
    auto input = part::io::byte_reader(fd).spawn();
    auto output = part::io::byte_writer(fd).spawn();
#else
    int rfd = fd;
    int wfd = ::dup(fd);
    if (wfd < 0) {
        io::close(fd);
        throw csp::error("dup failed");
    }
    auto input = part::io::byte_reader(rfd).spawn();
    auto output = part::io::byte_writer(wfd).spawn();
#endif

    connection c;
    c.fd = fd;
    c.input = std::move(input);
    c.output = std::move(output);
    c.remote_addr = std::move(remote);
    return c;
}

} // anonymous namespace

listener listen(uint16_t port, listen_options opts) {
    return listen("::", port, opts);
}

listener listen(const std::string& addr, uint16_t port,
                listen_options opts) {
    struct addrinfo hints {};
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    auto result = io::resolve(addr, std::to_string(port), &hints);
    if (!result) {
        throw csp::error(std::string("listen resolve failed: ") +
                         result.message());
    }

    auto* ai = result.info.get();
    io::socket_t listen_fd = ::socket(ai->ai_family, ai->ai_socktype,
                                       ai->ai_protocol);
    if (listen_fd == io::invalid_socket) {
        throw csp::error("socket failed");
    }

    if (opts.reuse_addr) {
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
    }

    if (opts.dual_stack && ai->ai_family == AF_INET6) {
        int off = 0;
        setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char*>(&off), sizeof(off));
    }

    if (::bind(listen_fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        io::close(listen_fd);
        throw csp::error("bind failed");
    }

    if (::listen(listen_fd, opts.backlog) < 0) {
        io::close(listen_fd);
        throw csp::error("listen failed");
    }

    io::set_nonblock(listen_fd);

    struct sockaddr_storage bound_addr {};
    socklen_t bound_len = sizeof(bound_addr);
    getsockname(listen_fd, reinterpret_cast<struct sockaddr*>(&bound_addr),
                &bound_len);
    auto local = format_addr(reinterpret_cast<struct sockaddr*>(&bound_addr),
                             bound_len);
    uint16_t actual_port = 0;
    if (bound_addr.ss_family == AF_INET6) {
        actual_port = ntohs(
            reinterpret_cast<struct sockaddr_in6*>(&bound_addr)->sin6_port);
    } else {
        actual_port = ntohs(
            reinterpret_cast<struct sockaddr_in*>(&bound_addr)->sin_port);
    }

    auto conns = spawn_producer<connection>(
        [listen_fd](writer<connection> out) {
            internal::descr("net/listen");

            // Sentinel: watches for reader death.  When the connections
            // reader dies, the sentinel exits and drops stop_w, which
            // fires the ~stop_r vulture in the accept loop.
            chan<> stop_ch;
            auto out_copy = out.copy();
            csp::spawn([out_copy = std::move(out_copy),
                         stop_w = std::move(stop_ch.w)] {
                internal::descr("net/listen/sentinel");
                prialt(~out_copy);
            });

            // Accept loop: multiplex listen fd readability with stop
            // signal via the reactor.  This avoids io::accept (which
            // blocks in wait_readable and can't be composed with a
            // channel stop signal).
            auto stop_r = std::move(stop_ch.r);
            auto fd_signal = detail::create_fd_readable(listen_fd);

            for (;;) {
                // Wait for either: listen fd readable, or stop signal.
                int k = prialt(~fd_signal, ~stop_r);
                if (k == ~1) break;  // stop signal — reader died.

                // fd is readable — try accept (non-blocking).
                struct sockaddr_storage client_addr {};
                socklen_t client_len = sizeof(client_addr);
                io::socket_t client_fd = ::accept(
                    listen_fd,
                    reinterpret_cast<struct sockaddr*>(&client_addr),
                    &client_len);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // Re-register for readability and retry.
                        fd_signal = detail::create_fd_readable(listen_fd);
                        continue;
                    }
                    break;  // real error
                }

                auto remote = format_addr(
                    reinterpret_cast<struct sockaddr*>(&client_addr),
                    client_len);

                if (!(out << make_connection(client_fd, std::move(remote))))
                    break;

                // Re-register for next connection.
                fd_signal = detail::create_fd_readable(listen_fd);
            }
            io::close(listen_fd);
        });

    return {std::move(conns), actual_port, std::move(local)};
}

connection dial(const std::string& host, uint16_t port) {
    return dial(host, std::to_string(port));
}

connection dial(const std::string& host, const std::string& service) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    auto result = io::resolve(host, service, &hints);
    if (!result) {
        throw csp::error(std::string("dial resolve failed: ") +
                         result.message());
    }

    for (auto* ai = result.info.get(); ai; ai = ai->ai_next) {
        io::socket_t fd = ::socket(ai->ai_family, ai->ai_socktype,
                                    ai->ai_protocol);
        if (fd == io::invalid_socket) continue;

        io::set_nonblock(fd);

        if (io::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            auto remote = format_addr(ai->ai_addr, ai->ai_addrlen);
            return make_connection(fd, std::move(remote));
        }

        io::close(fd);
    }

    throw csp::error("dial failed: all addresses exhausted");
}

} // namespace csp::net
