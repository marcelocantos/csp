#include <csp/net.h>
#include <csp/cancel.h>
#include <csp/internal/signal.h>

#include <cstring>
#include <mutex>

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

#ifdef _WIN32
// socket()/bind() before any reactor activity still need Winsock.
// Reactor::ensure_started also calls WSAStartup (ref-counted, safe).
void ensure_winsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}
#else
void ensure_winsock() {}
#endif

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
    // Windows can't dup a SOCKET via POSIX dup(), so the read source and
    // the writer share the one fd.  The writer owns the close; the source
    // is a non-owning view over the same fd.
    auto output = part::io::byte_writer(fd).spawn();
    io::source src = io::fd_source_view(fd);
#else
    // The source owns the original fd and closes it on EOF/exit.  The
    // writer gets its own dup so it can shutdown(SHUT_WR) before close —
    // making the peer's reader see EOF — without disturbing the source's
    // reads on the original fd.
    int raw_wfd = ::dup(fd.raw());
    if (raw_wfd < 0) {
        io::close(fd);
        throw csp::error("dup failed");
    }
    auto wfd = io::fd_t(raw_wfd);
    io::set_nonblock(wfd);
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
    io::source src = io::fd_source(fd);
#endif

    connection c;
    c.fd = fd;
    c.source = std::move(src);
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
    ensure_winsock();
    // AF_UNSPEC so IPv4 literals (127.0.0.1) and IPv6 (::) both resolve.
    // Hard-coding AF_INET6 made listen("127.0.0.1") fail resolve and left
    // dialers blocked forever on the port channel (Windows full-suite hang
    // after T38; macOS terminate without RunStats). 🎯T39
    // AI_PASSIVE only for wildcard bind addresses — with a concrete host
    // (127.0.0.1) it can yield non-connectable results on Windows.
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (addr.empty() || addr == "::" || addr == "0.0.0.0"
        || addr == "0:0:0:0:0:0:0:0") {
        hints.ai_flags = AI_PASSIVE;
    }

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

    if (::bind(listen_fd.raw(), ai->ai_addr, static_cast<int>(ai->ai_addrlen)) < 0) {
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
    ensure_winsock();
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

        if (io::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) == 0) {
            auto remote = format_addr(ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
            return make_connection(fd, std::move(remote));
        }

        io::close(fd);
    }

    throw csp::error("dial failed: all addresses exhausted");
}

// --- Unified server (🎯T23.1) -----------------------------------------
//
// Walks the option list, calling each `apply()` so the protocol can stash
// its server handle into `out.protocol_servers`. The front-door TU never
// references any `csp::<proto>::` symbol by name — every per-protocol
// behaviour is reached through the function pointer in `protocol_option`.
// That keeps Rule 5 of the per-protocol DCE model intact: an `enable()`
// the user never calls leaves its protocol TU dead, and the linker drops
// the third-party libraries that protocol depended on.

server serve(uint16_t port, std::initializer_list<protocol_option> opts) {
    server out{.port = port, .protocol_servers = {}};
    apply_context ctx{.port = port, .out = &out};
    for (const auto& opt : opts) {
        if (opt.apply != nullptr) {
            opt.apply(ctx, opt.config);
        }
        if (opt.destroy != nullptr) {
            opt.destroy(opt.config);
        }
    }
    return out;
}

} // namespace csp::net
