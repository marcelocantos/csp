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

connection make_connection(io::fd_t fd, std::string remote) {
    // fd is already non-blocking (set by io::accept or net::dial).

#ifdef _WIN32
    auto input = part::io::byte_reader(fd).spawn();
    auto output = part::io::byte_writer(fd).spawn();
#else
    auto rfd = fd;
    int raw_wfd = ::dup(fd.raw());
    if (raw_wfd < 0) {
        io::close(fd);
        throw csp::error("dup failed");
    }
    auto wfd = io::fd_t(raw_wfd);
    io::set_nonblock(wfd);
    auto input = part::io::byte_reader(rfd).spawn();
    // Socket-aware byte_writer: shutdown(SHUT_WR) before close so
    // the peer's byte_reader sees EOF even though rfd still holds a
    // reference to the underlying socket.
    auto output = spawn_consumer<bytes>(
        [wfd](reader<bytes> in) {
            internal::descr("byte_writer");
            for (bytes chunk; in >> chunk;) {
                if (csp::io::write(wfd, chunk.data(), chunk.size()) < 0)
                    break;
            }
            ::shutdown(wfd.raw(), SHUT_WR);
            io::close(wfd);
        });
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
    io::fd_t listen_fd(::socket(ai->ai_family, ai->ai_socktype,
                                 ai->ai_protocol));
    if (!listen_fd) {
        throw csp::error("socket failed");
    }

    if (opts.reuse_addr) {
        int opt = 1;
        setsockopt(listen_fd.raw(), SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
    }

    if (opts.dual_stack && ai->ai_family == AF_INET6) {
        int off = 0;
        setsockopt(listen_fd.raw(), IPPROTO_IPV6, IPV6_V6ONLY,
                   reinterpret_cast<const char*>(&off), sizeof(off));
    }

    if (::bind(listen_fd.raw(), ai->ai_addr, ai->ai_addrlen) < 0) {
        io::close(listen_fd);
        throw csp::error("bind failed");
    }

    if (::listen(listen_fd.raw(), opts.backlog) < 0) {
        io::close(listen_fd);
        throw csp::error("listen failed");
    }

    io::set_nonblock(listen_fd);

    struct sockaddr_storage bound_addr {};
    socklen_t bound_len = sizeof(bound_addr);
    getsockname(listen_fd.raw(), reinterpret_cast<struct sockaddr*>(&bound_addr),
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

    // The accept loop uses cancellation for clean shutdown.
    // The sentinel watches for reader death and cancels the scope.
    // The cancel_guard is owned by the producer imp (not shared).
    // The sentinel signals cancellation through the cancel_guard's
    // operator() — but to avoid sharing the guard across imps
    // (which causes csp::local lifecycle issues), we use a channel
    // to signal the producer, which then cancels itself.

    auto conns = spawn_producer<connection>(
        [listen_fd](writer<connection> out) {
            internal::descr("net/listen");

            // The producer owns the cancel scope.
            auto guard = cancellation();

            // Sentinel signals stop via a channel.
            chan<> stop_ch;
            auto out_copy = out.copy();
            csp::spawn([out_copy = std::move(out_copy),
                         stop_w = std::move(stop_ch.w)] {
                internal::descr("net/listen/sentinel");
                prialt(~out_copy);
                // stop_w dropped here → stop_r dies.
            });

            // Stopper imp: watches stop channel, triggers cancellation.
            // Runs inside the same cancel scope but only does a prialt
            // on stop_r, then cancels the guard.  Because the guard is
            // on the producer's stack, we pass a raw pointer — safe
            // because the producer outlives the stopper (producer waits
            // in the accept loop until cancelled).
            auto stop_r = std::move(stop_ch.r);
            auto* guard_ptr = &guard;
            csp::spawn([stop_r = std::move(stop_r), guard_ptr] {
                internal::descr("net/listen/stopper");
                prialt(~stop_r);
                (*guard_ptr)();
            });

            try {
                for (;;) {
                    struct sockaddr_storage client_addr {};
                    socklen_t client_len = sizeof(client_addr);
                    io::fd_t client_fd = io::accept(
                        listen_fd,
                        reinterpret_cast<struct sockaddr*>(&client_addr),
                        &client_len);
                    if (!client_fd) continue;

                    auto remote = format_addr(
                        reinterpret_cast<struct sockaddr*>(&client_addr),
                        client_len);

                    if (!(out << make_connection(client_fd, std::move(remote))))
                        break;
                }
            } catch (canceled const&) {
                // Stop signal received.
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
        io::fd_t fd(::socket(ai->ai_family, ai->ai_socktype,
                              ai->ai_protocol));
        if (!fd) continue;

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
