#include <csp/net.h>
#include <csp/cancel.h>
#include <csp/internal/signal.h>

#include <cctype>
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

// --- Shared listener setup (🎯T48) -------------------------------------

listen_result bind_listener(const std::string& addr, uint16_t port,
                            const listen_options& opts,
                            const std::string& resolve_err_prefix) {
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
        throw csp::error(resolve_err_prefix + result.message());
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

    if (::bind(listen_fd.raw(), ai->ai_addr,
               static_cast<int>(ai->ai_addrlen)) < 0) {
        io::close(listen_fd);
        throw csp::error("bind failed");
    }

    if (::listen(listen_fd.raw(), opts.backlog) < 0) {
        io::close(listen_fd);
        throw csp::error("listen failed");
    }

    io::set_nonblock(listen_fd);

    struct sockaddr_storage bound {};
    socklen_t bound_len = sizeof(bound);
    getsockname(listen_fd.raw(),
                reinterpret_cast<struct sockaddr*>(&bound), &bound_len);
    auto local = io::format_addr(
        reinterpret_cast<struct sockaddr*>(&bound), bound_len);

    uint16_t actual_port = 0;
    if (bound.ss_family == AF_INET6) {
        actual_port = ntohs(
            reinterpret_cast<struct sockaddr_in6*>(&bound)->sin6_port);
    } else {
        actual_port = ntohs(
            reinterpret_cast<struct sockaddr_in*>(&bound)->sin_port);
    }

    // A wildcard bind address is not connectable; substitute the loopback
    // of the same family, keeping the port the kernel assigned.
    sockaddr_storage wake = bound;
    if (wake.ss_family == AF_INET6) {
        auto* w6 = reinterpret_cast<struct sockaddr_in6*>(&wake);
        if (IN6_IS_ADDR_UNSPECIFIED(&w6->sin6_addr)) {
            w6->sin6_addr = in6addr_loopback;
        }
    } else {
        auto* w4 = reinterpret_cast<struct sockaddr_in*>(&wake);
        if (w4->sin_addr.s_addr == htonl(INADDR_ANY)) {
            w4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        }
    }

    return {listen_fd, actual_port, std::move(local), wake, bound_len};
}

void wake_listener(const sockaddr_storage& addr, socklen_t len) {
    if (len == 0) return;
    auto* sa = reinterpret_cast<const sockaddr*>(&addr);
#ifndef _WIN32
    int raw = ::socket(addr.ss_family, SOCK_STREAM, 0);
    if (raw < 0) return;
    ::connect(raw, sa, len);
    ::close(raw);
#else
    SOCKET raw = ::socket(addr.ss_family, SOCK_STREAM, 0);
    if (raw == INVALID_SOCKET) return;
    ::connect(raw, sa, static_cast<int>(len));
    closesocket(raw);
#endif
}

// --- URL authority parsing (🎯T48) -------------------------------------

authority parse_authority(std::string_view url,
                          std::string_view scheme_prefix,
                          uint16_t default_port) {
    auto ieq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    bool scheme_ok = url.size() >= scheme_prefix.size();
    if (scheme_ok) {
        for (size_t i = 0; i < scheme_prefix.size(); ++i) {
            if (!ieq(url[i], scheme_prefix[i])) {
                scheme_ok = false;
                break;
            }
        }
    }
    if (!scheme_ok) {
        throw csp::error("unsupported URL scheme (only " +
                         std::string(scheme_prefix) + " is supported)");
    }

    auto rest = url.substr(scheme_prefix.size());
    authority result;
    result.port = default_port;

    // Split host[:port] from path.
    auto slash = rest.find('/');
    std::string_view auth;
    if (slash == std::string_view::npos) {
        auth = rest;
    } else {
        auth = rest.substr(0, slash);
        result.path = std::string(rest.substr(slash));
    }

    // Handle [IPv6]:port
    if (!auth.empty() && auth[0] == '[') {
        auto bracket = auth.find(']');
        if (bracket == std::string_view::npos) {
            throw csp::error("malformed IPv6 address in URL");
        }
        result.host = std::string(auth.substr(1, bracket - 1));
        if (bracket + 1 < auth.size() && auth[bracket + 1] == ':') {
            result.port = static_cast<uint16_t>(
                std::stoul(std::string(auth.substr(bracket + 2))));
        }
    } else {
        auto colon = auth.rfind(':');
        if (colon == std::string_view::npos) {
            result.host = std::string(auth);
        } else {
            result.host = std::string(auth.substr(0, colon));
            result.port = static_cast<uint16_t>(
                std::stoul(std::string(auth.substr(colon + 1))));
        }
    }

    if (result.host.empty()) {
        throw csp::error("empty host in URL");
    }
    return result;
}

listener listen(uint16_t port, listen_options opts) {
    return listen("::", port, opts);
}

listener listen(const std::string& addr, uint16_t port,
                listen_options opts) {
    auto lr = bind_listener(addr, port, opts, "listen resolve failed: ");
    auto listen_fd = lr.listen_fd;

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

                    auto remote = io::format_addr(
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

    return {std::move(conns), lr.port, std::move(lr.local_addr)};
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
            auto remote = io::format_addr(ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
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
