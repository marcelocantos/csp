// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <csp/http2.h>
#include <csp/io.h>
#include <csp/cancel.h>

#ifdef CSP_TLS
#include <csp/tls.h>
#endif

extern "C" {
#include <nghttp2/nghttp2.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <ws2tcpip.h>
#endif

namespace csp::http2 {

namespace {

// --- Utilities ---

static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

static std::string to_lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static http::method method_from_str(const std::string& s) {
    if (s == "GET")     return http::method::GET;
    if (s == "HEAD")    return http::method::HEAD;
    if (s == "POST")    return http::method::POST;
    if (s == "PUT")     return http::method::PUT;
    if (s == "DELETE")  return http::method::DELETE_;
    if (s == "PATCH")   return http::method::PATCH;
    if (s == "OPTIONS") return http::method::OPTIONS;
    if (s == "CONNECT") return http::method::CONNECT;
    if (s == "TRACE")   return http::method::TRACE;
    return http::method::GET;
}

static std::string format_addr(const struct sockaddr* sa, socklen_t len) {
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    int rc = getnameinfo(sa, len, host, sizeof(host), serv, sizeof(serv),
                         NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) return "unknown";
    if (sa->sa_family == AF_INET6)
        return std::string("[") + host + "]:" + serv;
    return std::string(host) + ":" + serv;
}

// --- I/O abstraction ---
//
// Allows the same HTTP/2 session loop to work over raw fds (h2c) or TLS conns.

struct io_handle {
    virtual ~io_handle() = default;
    // Read up to len plaintext bytes into buf. Returns bytes read, 0 on EOF,
    // negative on error.
    virtual ssize_t read(uint8_t* buf, size_t len) = 0;
    // Write len bytes from buf. Returns bytes written, negative on error.
    virtual ssize_t write(const uint8_t* buf, size_t len) = 0;
    // Close the underlying connection.
    virtual void close() = 0;
};

// --- Raw (cleartext) I/O handle ---

struct raw_io : io_handle {
    io::fd_t fd;
    explicit raw_io(io::fd_t f) : fd(f) {}

    ssize_t read(uint8_t* buf, size_t len) override {
        return io::read(fd, buf, len);
    }
    ssize_t write(const uint8_t* buf, size_t len) override {
        return io::write(fd, buf, len);
    }
    void close() override { io::close(fd); }
};

#ifdef CSP_TLS

// --- TLS I/O handle ---
//
// Wraps a csp::tls::conn. The conn has already completed handshake by the
// time this is created.

struct tls_io : io_handle {
    tls::conn conn_;
    explicit tls_io(tls::conn c) : conn_(std::move(c)) {}

    ssize_t read(uint8_t* buf, size_t len) override {
        return conn_.read(buf, len);
    }
    ssize_t write(const uint8_t* buf, size_t len) override {
        return conn_.write(buf, len);
    }
    void close() override {
        try { conn_.shutdown(); } catch (...) {}
    }
};

#endif // CSP_TLS

// --- Per-stream state ---

struct stream_state {
    int32_t id = 0;
    std::string method;
    std::string path;
    std::string scheme;
    std::string authority;
    std::vector<std::pair<std::string, std::string>> headers;
    csp::bytes body;
};

// --- nghttp2 session state ---

struct session_state {
    nghttp2_session* session = nullptr;
    io_handle* io = nullptr;  // non-owning; owned by caller
    std::map<int32_t, stream_state> streams;
    writer<http::request> stream_out;
    bool want_close = false;
};

// --- Data provider for response body ---

struct body_data {
    csp::bytes body;
    size_t offset = 0;
};

static nghttp2_ssize body_read_callback(nghttp2_session* /*session*/,
                                        int32_t /*stream_id*/,
                                        uint8_t* buf, size_t length,
                                        uint32_t* data_flags,
                                        nghttp2_data_source* source,
                                        void* /*user_data*/) {
    auto* bd = static_cast<body_data*>(source->ptr);
    size_t remaining = bd->body.size() - bd->offset;
    size_t n = std::min(length, remaining);
    if (n > 0) {
        std::memcpy(buf, bd->body.data() + bd->offset, n);
        bd->offset += n;
    }
    if (bd->offset >= bd->body.size()) {
        *data_flags = NGHTTP2_DATA_FLAG_EOF;
    }
    return static_cast<nghttp2_ssize>(n);
}

// --- Flush pending frames from nghttp2 to the I/O handle ---

static bool flush_session(session_state* st) {
    for (;;) {
        const uint8_t* data = nullptr;
        ssize_t n = nghttp2_session_mem_send(st->session, &data);
        if (n == 0) break;
        if (n < 0) return false;
        if (st->io->write(data, static_cast<size_t>(n)) < 0) return false;
    }
    return true;
}

// --- nghttp2 callbacks ---

static int on_begin_headers(nghttp2_session* /*session*/,
                            const nghttp2_frame* frame,
                            void* user_data) {
    auto* st = static_cast<session_state*>(user_data);
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        auto& s = st->streams[frame->hd.stream_id];
        s.id = frame->hd.stream_id;
    }
    return 0;
}

static int on_header(nghttp2_session* /*session*/,
                     const nghttp2_frame* frame,
                     const uint8_t* name, size_t namelen,
                     const uint8_t* value, size_t valuelen,
                     uint8_t /*flags*/,
                     void* user_data) {
    auto* st = static_cast<session_state*>(user_data);
    if (frame->hd.type != NGHTTP2_HEADERS ||
        frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return 0;
    }

    auto it = st->streams.find(frame->hd.stream_id);
    if (it == st->streams.end()) return 0;

    auto& s = it->second;
    std::string k(reinterpret_cast<const char*>(name), namelen);
    std::string v(reinterpret_cast<const char*>(value), valuelen);

    if (k == ":method")    { s.method    = v; }
    else if (k == ":path") { s.path      = v; }
    else if (k == ":scheme") { s.scheme  = v; }
    else if (k == ":authority") { s.authority = v; }
    else { s.headers.emplace_back(std::move(k), std::move(v)); }
    return 0;
}

static int on_data_chunk_recv(nghttp2_session* /*session*/,
                              uint8_t /*flags*/,
                              int32_t stream_id,
                              const uint8_t* data, size_t len,
                              void* user_data) {
    auto* st = static_cast<session_state*>(user_data);
    auto it = st->streams.find(stream_id);
    if (it != st->streams.end()) {
        auto& body = it->second.body;
        body.insert(body.end(), data, data + len);
    }
    return 0;
}

// --- Submit response for a completed stream ---
//
// Called synchronously inside on_frame_recv. Waits for the handler to
// respond, then submits the response to nghttp2.
//
// Design note: this blocks the read loop while waiting for the handler.
// For HTTP/2 multiplexing, requests are dispatched to the handler via the
// stream_out channel. The handler (consumer of the endpoint) is expected
// to process them. Because nghttp2 session is not thread-safe, all
// session operations happen in this single imp (the read loop). Multiple
// streams can be interleaved at the framing level, but each stream's
// request-response cycle proceeds independently.
//
// A production server would want per-stream imps for concurrent handling.
// This initial implementation serialises response submission per stream
// to keep session state single-threaded, which is correct but limits
// concurrency. Future work: track pending response channels and poll them
// with prialt to submit responses as they arrive.

static void dispatch_stream(session_state* st, int32_t stream_id) {
    auto it = st->streams.find(stream_id);
    if (it == st->streams.end()) return;

    stream_state ss = std::move(it->second);
    st->streams.erase(it);

    if (ss.method.empty() || ss.path.empty()) {
        nghttp2_submit_rst_stream(st->session, NGHTTP2_FLAG_NONE,
                                  stream_id, NGHTTP2_PROTOCOL_ERROR);
        return;
    }

    // Build the request.
    chan<http::response> resp_ch;
    http::request req;
    req.method = method_from_str(ss.method);
    req.url = ss.path;
    req.version_major = 2;
    req.version_minor = 0;
    req.headers = std::move(ss.headers);
    req.body = std::move(ss.body);
    req.keep_alive = true;
    req.respond = std::move(resp_ch.w);

    // Deliver request to handler.
    if (!(st->stream_out << std::move(req))) {
        nghttp2_submit_rst_stream(st->session, NGHTTP2_FLAG_NONE,
                                  stream_id, NGHTTP2_INTERNAL_ERROR);
        return;
    }

    // Wait for response.
    http::response resp;
    if (!(resp_ch.r >> resp)) {
        resp.status = 500;
    }

    // Build response headers (lowercase, :status first).
    std::string status_str = std::to_string(resp.status);
    std::vector<nghttp2_nv> nva;
    nva.reserve(resp.headers.size() + 2);

    nva.push_back({
        reinterpret_cast<uint8_t*>(const_cast<char*>(":status")),
        reinterpret_cast<uint8_t*>(const_cast<char*>(status_str.c_str())),
        7, status_str.size(),
        NGHTTP2_NV_FLAG_NO_COPY_NAME,
    });

    bool has_cl = false;
    for (auto& [k, v] : resp.headers) {
        if (iequals(k, "content-length")) has_cl = true;
    }
    std::string cl_str;
    if (!resp.body.empty() && !has_cl) {
        cl_str = std::to_string(resp.body.size());
        nva.push_back({
            reinterpret_cast<uint8_t*>(const_cast<char*>("content-length")),
            reinterpret_cast<uint8_t*>(const_cast<char*>(cl_str.c_str())),
            14, cl_str.size(),
            NGHTTP2_NV_FLAG_NO_COPY_NAME,
        });
    }

    std::vector<std::string> lower_keys;
    lower_keys.reserve(resp.headers.size());
    for (auto& [k, v] : resp.headers) lower_keys.push_back(to_lower(k));
    for (size_t i = 0; i < resp.headers.size(); ++i) {
        auto& [k, v] = resp.headers[i];
        nva.push_back({
            reinterpret_cast<uint8_t*>(
                const_cast<char*>(lower_keys[i].c_str())),
            reinterpret_cast<uint8_t*>(const_cast<char*>(v.c_str())),
            lower_keys[i].size(), v.size(),
            NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE,
        });
    }

    if (resp.body.empty()) {
        nghttp2_submit_response2(st->session, stream_id,
                                 nva.data(), nva.size(), nullptr);
        flush_session(st);
    } else {
        // Body is heap-allocated; we free it after flush completes.
        // nghttp2 calls body_read_callback synchronously from
        // nghttp2_session_mem_send, so the body_data lives for the
        // duration of flush_session.
        auto bd = std::make_unique<body_data>(body_data{std::move(resp.body), 0});
        nghttp2_data_provider2 prd;
        prd.source.ptr = bd.get();
        prd.read_callback = body_read_callback;
        nghttp2_submit_response2(st->session, stream_id,
                                 nva.data(), nva.size(), &prd);
        flush_session(st);
        // bd goes out of scope and is freed here.
    }
}

static int on_frame_recv(nghttp2_session* /*session*/,
                         const nghttp2_frame* frame,
                         void* user_data) {
    auto* st = static_cast<session_state*>(user_data);

    if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
        switch (frame->hd.type) {
        case NGHTTP2_HEADERS:
        case NGHTTP2_DATA:
            dispatch_stream(st, frame->hd.stream_id);
            break;
        default:
            break;
        }
    }

    if (frame->hd.type == NGHTTP2_GOAWAY) {
        st->want_close = true;
    }

    return 0;
}

static int on_stream_close(nghttp2_session* /*session*/,
                           int32_t stream_id,
                           uint32_t /*error_code*/,
                           void* user_data) {
    auto* st = static_cast<session_state*>(user_data);
    st->streams.erase(stream_id);
    return 0;
}

// --- HTTP/2 connection handler ---
//
// Takes ownership of the io_handle. Runs the nghttp2 session loop until
// EOF or error.

void handle_h2_connection(std::unique_ptr<io_handle> ioh,
                          std::string remote_addr,
                          writer<endpoint> ep_out,
                          size_t read_chunk_size) {
    internal::descr("http2/conn");

    chan<http::request> stream_ch;

    endpoint ep;
    ep.streams = std::move(stream_ch.r);
    ep.remote_addr = std::move(remote_addr);
    if (!(ep_out << std::move(ep))) {
        ioh->close();
        return;
    }

    nghttp2_session_callbacks* cbs = nullptr;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, on_begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(cbs, on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_recv);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close);

    session_state st;
    st.io = ioh.get();
    st.stream_out = std::move(stream_ch.w);

    int rc = nghttp2_session_server_new(&st.session, cbs, &st);
    nghttp2_session_callbacks_del(cbs);

    if (rc != 0) {
        ioh->close();
        return;
    }

    // Send server connection preface (initial SETTINGS).
    nghttp2_settings_entry iv[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
    };
    nghttp2_submit_settings(st.session, NGHTTP2_FLAG_NONE, iv, 1);
    flush_session(&st);

    // Read loop.
    std::vector<uint8_t> buf(read_chunk_size);
    for (;;) {
        ssize_t n = ioh->read(buf.data(), buf.size());
        if (n <= 0) break;

        nghttp2_ssize processed = nghttp2_session_mem_recv2(
            st.session, buf.data(), static_cast<size_t>(n));
        if (processed < 0) break;

        if (!flush_session(&st)) break;

        if (st.want_close ||
            (!nghttp2_session_want_read(st.session) &&
             !nghttp2_session_want_write(st.session))) {
            break;
        }
    }

    nghttp2_session_terminate_session(st.session, NGHTTP2_NO_ERROR);
    flush_session(&st);
    nghttp2_session_del(st.session);
    ioh->close();
}

// --- TCP listener setup ---

struct listen_result {
    io::fd_t listen_fd;
    uint16_t port;
    std::string local_addr;
    // Address to self-connect to when waking a blocked accept (see
    // wake_listener). Derived from getsockname, so it tracks whatever
    // family and address the listener actually bound.
    sockaddr_storage wake_addr{};
    socklen_t wake_len = 0;
};

// Unblock an accept() by opening a throwaway connection to the listener.
// The address must match the listener's own family: a listener bound to
// 127.0.0.1 is unreachable over ::1, and vice versa.
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

listen_result create_listener(const std::string& addr, uint16_t port,
                              const serve_options& opts) {
    // AF_UNSPEC so IPv4 literals (127.0.0.1) and IPv6 (::) both resolve;
    // AI_PASSIVE only for wildcard bind addresses (🎯T39 — see net.cc).
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (addr.empty() || addr == "::" || addr == "0.0.0.0"
        || addr == "0:0:0:0:0:0:0:0") {
        hints.ai_flags = AI_PASSIVE;
    }

    auto result = io::resolve(addr, std::to_string(port), &hints);
    if (!result) {
        throw csp::error(std::string("http2 listen resolve failed: ") +
                         result.message());
    }

    auto* ai = result.info.get();
    io::fd_t listen_fd(::socket(ai->ai_family, ai->ai_socktype,
                                 ai->ai_protocol));
    if (!listen_fd) throw csp::error("socket failed");

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

    struct sockaddr_storage bound{};
    socklen_t bound_len = sizeof(bound);
    getsockname(listen_fd.raw(),
                reinterpret_cast<struct sockaddr*>(&bound), &bound_len);
    auto local = format_addr(
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

// --- Accept loop: cleartext h2c ---

server do_serve(const std::string& addr, uint16_t port,
                const serve_options& opts) {
    auto lr = create_listener(addr, port, opts);

    auto endpoints = spawn_producer<endpoint>(
        [listen_fd = lr.listen_fd, wake_addr = lr.wake_addr,
         wake_len = lr.wake_len, opts](writer<endpoint> out) {
            internal::descr("http2/serve");

            auto stop = std::make_shared<std::atomic<bool>>(false);
            auto out_copy = out.copy();
            auto stop_flag = stop;
            csp::spawn([out_copy = std::move(out_copy),
                         stop_flag, wake_addr, wake_len] {
                internal::descr("http2/serve/sentinel");
                prialt(~out_copy);
                stop_flag->store(true, std::memory_order_release);
                wake_listener(wake_addr, wake_len);
            });

            for (;;) {
                struct sockaddr_storage client_addr{};
                socklen_t client_len = sizeof(client_addr);
                io::fd_t client_fd = io::accept(
                    listen_fd,
                    reinterpret_cast<struct sockaddr*>(&client_addr),
                    &client_len);

                if (!client_fd ||
                    stop->load(std::memory_order_acquire)) {
                    if (client_fd) io::close(client_fd);
                    break;
                }

                auto remote = format_addr(
                    reinterpret_cast<struct sockaddr*>(&client_addr),
                    client_len);

                auto out_copy2 = out.copy();
                csp::spawn([fd = client_fd,
                            r = std::move(remote),
                            o = std::move(out_copy2),
                            chunk = opts.read_chunk_size]() mutable {
                    handle_h2_connection(
                        std::make_unique<raw_io>(fd),
                        std::move(r), std::move(o), chunk);
                });
            }
            io::close(listen_fd);
        });

    return {std::move(endpoints), lr.port, std::move(lr.local_addr)};
}

} // anonymous namespace

// --- Public API: cleartext ---

server serve(uint16_t port, serve_options opts) {
    return serve("::", port, opts);
}

server serve(const std::string& addr, uint16_t port, serve_options opts) {
    return do_serve(addr, port, opts);
}

// --- Public API: TLS ---

#ifdef CSP_TLS

namespace {

server do_serve_tls(const std::string& addr, uint16_t port,
                    const char* cert_pem, const char* key_pem,
                    const serve_options& opts) {
    auto lr = create_listener(addr, port, opts);

    // TLS context is shared across all accepted connections.
    // It is immutable after setup.
    auto ctx = std::make_shared<tls::context>(tls::context::server);
    ctx->load_cert(cert_pem);
    ctx->load_key(key_pem);

    auto endpoints = spawn_producer<endpoint>(
        [listen_fd = lr.listen_fd, wake_addr = lr.wake_addr,
         wake_len = lr.wake_len, opts, ctx](
            writer<endpoint> out) mutable {
            internal::descr("http2/tls/serve");

            auto stop = std::make_shared<std::atomic<bool>>(false);
            auto out_copy = out.copy();
            auto stop_flag = stop;
            csp::spawn([out_copy = std::move(out_copy),
                         stop_flag, wake_addr, wake_len] {
                internal::descr("http2/tls/serve/sentinel");
                prialt(~out_copy);
                stop_flag->store(true, std::memory_order_release);
                wake_listener(wake_addr, wake_len);
            });

            for (;;) {
                struct sockaddr_storage client_addr{};
                socklen_t client_len = sizeof(client_addr);
                io::fd_t client_fd = io::accept(
                    listen_fd,
                    reinterpret_cast<struct sockaddr*>(&client_addr),
                    &client_len);

                if (!client_fd ||
                    stop->load(std::memory_order_acquire)) {
                    if (client_fd) io::close(client_fd);
                    break;
                }

                auto remote = format_addr(
                    reinterpret_cast<struct sockaddr*>(&client_addr),
                    client_len);

                auto out_copy2 = out.copy();
                csp::spawn([fd = client_fd,
                            r = std::move(remote),
                            o = std::move(out_copy2),
                            ctx, chunk = opts.read_chunk_size]() mutable {
                    internal::descr("http2/tls/handshake");
                    // Perform TLS handshake.
                    // tls::conn takes ownership of the fd lifecycle for TLS.
                    tls::conn c(*ctx, fd);
                    try {
                        c.handshake();
                    } catch (...) {
                        // Handshake failed — close and discard.
                        io::close(fd);
                        return;
                    }
                    // Hand off to HTTP/2 session loop via TLS I/O handle.
                    // tls_io takes ownership of conn.
                    handle_h2_connection(
                        std::make_unique<tls_io>(std::move(c)),
                        std::move(r), std::move(o), chunk);
                });
            }
            io::close(listen_fd);
        });

    return {std::move(endpoints), lr.port, std::move(lr.local_addr)};
}

} // anonymous namespace

server serve_tls(uint16_t port, const char* cert_pem, const char* key_pem,
                 serve_options opts) {
    return serve_tls("::", port, cert_pem, key_pem, opts);
}

server serve_tls(const std::string& addr, uint16_t port,
                 const char* cert_pem, const char* key_pem,
                 serve_options opts) {
    return do_serve_tls(addr, port, cert_pem, key_pem, opts);
}

#endif // CSP_TLS

} // namespace csp::http2
