// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <csp/http.h>
#include <csp/cancel.h>
#include <csp/internal/signal.h>
#include <csp/source.h>

#include <llhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <ws2tcpip.h>
#endif

namespace csp::http {

// --- method_name ---

const char* method_name(method m) {
    switch (m) {
    case method::GET:      return "GET";
    case method::HEAD:     return "HEAD";
    case method::POST:     return "POST";
    case method::PUT:      return "PUT";
    case method::DELETE_:  return "DELETE";
    case method::PATCH:    return "PATCH";
    case method::OPTIONS:  return "OPTIONS";
    case method::CONNECT:  return "CONNECT";
    case method::TRACE:    return "TRACE";
    }
    return "UNKNOWN";
}

// --- Internal helpers ---

namespace {

bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

method from_llhttp(llhttp_method_t m) {
    switch (m) {
    case HTTP_GET:     return method::GET;
    case HTTP_HEAD:    return method::HEAD;
    case HTTP_POST:    return method::POST;
    case HTTP_PUT:     return method::PUT;
    case HTTP_DELETE:  return method::DELETE_;
    case HTTP_PATCH:   return method::PATCH;
    case HTTP_OPTIONS: return method::OPTIONS;
    case HTTP_CONNECT: return method::CONNECT;
    case HTTP_TRACE:   return method::TRACE;
    default:           return method::GET;
    }
}

std::string status_text(int code) {
    switch (code) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

bytes serialize_response(const response& resp) {
    std::ostringstream os;
    os << "HTTP/1.1 " << resp.status << " " << status_text(resp.status)
       << "\r\n";

    bool has_content_length = false;
    for (auto& [k, v] : resp.headers) {
        os << k << ": " << v << "\r\n";
        if (iequals(k, "Content-Length")) has_content_length = true;
    }
    if (!has_content_length) {
        os << "Content-Length: " << resp.body.size() << "\r\n";
    }
    os << "\r\n";

    auto head = os.str();
    bytes out;
    out.reserve(head.size() + resp.body.size());
    out.insert(out.end(), head.begin(), head.end());
    out.insert(out.end(), resp.body.begin(), resp.body.end());
    return out;
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

// --- Per-connection parser state ---

struct parse_state {
    std::string url;
    std::string header_field;
    std::string header_value;
    std::vector<std::pair<std::string, std::string>> headers;
    // Body bytes produced by the current llhttp_execute call, drained to
    // the handler's body_stream after each execute.  Bounded by the read
    // chunk size — the whole body is never accumulated here (🎯T17.5).
    bytes body_stage;
    bool in_value = false;

    llhttp_method_t req_method = HTTP_GET;
    uint8_t version_major = 1;
    uint8_t version_minor = 1;
    bool keep_alive = true;

    bool headers_complete = false;
    bool message_complete = false;

    void flush_header() {
        if (in_value) {
            headers.emplace_back(std::move(header_field),
                                 std::move(header_value));
            header_field.clear();
            header_value.clear();
            in_value = false;
        }
    }

    void reset() {
        url.clear();
        headers.clear();
        body_stage.clear();
        headers_complete = false;
        message_complete = false;
    }
};

// --- llhttp callbacks ---

int on_url(llhttp_t* p, const char* at, size_t len) {
    static_cast<parse_state*>(p->data)->url.append(at, len);
    return 0;
}

int on_header_field(llhttp_t* p, const char* at, size_t len) {
    auto* st = static_cast<parse_state*>(p->data);
    st->flush_header();
    st->header_field.append(at, len);
    return 0;
}

int on_header_value(llhttp_t* p, const char* at, size_t len) {
    auto* st = static_cast<parse_state*>(p->data);
    st->in_value = true;
    st->header_value.append(at, len);
    return 0;
}

int on_headers_complete(llhttp_t* p) {
    auto* st = static_cast<parse_state*>(p->data);
    st->flush_header();
    st->req_method = static_cast<llhttp_method_t>(p->method);
    st->version_major = p->http_major;
    st->version_minor = p->http_minor;
    st->keep_alive = llhttp_should_keep_alive(p);
    st->headers_complete = true;
    return 0;
}

int on_body(llhttp_t* p, const char* at, size_t len) {
    auto* st = static_cast<parse_state*>(p->data);
    st->body_stage.insert(st->body_stage.end(),
                          reinterpret_cast<const uint8_t*>(at),
                          reinterpret_cast<const uint8_t*>(at) + len);
    return 0;
}

int on_message_complete(llhttp_t* p) {
    static_cast<parse_state*>(p->data)->message_complete = true;
    llhttp_pause(p);
    return 0;
}

// --- Connection handler imp ---
//
// Uses direct I/O on the fd rather than net::connection channels.
// This avoids inheriting cancel scopes from the accept loop.
//
// Reads go through an io::source (🎯T17.1) built over the fd as a
// non-owning view.  handle_connection retains fd ownership for the
// explicit close on completion or for the WebSocket-upgrade hijack
// handoff after dropping the source.
//
// Body streaming (🎯T17.5): the request is delivered to the handler as
// soon as its headers are parsed, before the body has finished arriving.
// Body bytes are handed to the handler's body_stream chunk-by-chunk as
// llhttp's on_body callback fires, so memory stays bounded by the read
// chunk size rather than the body size.
//
// Robustness to early responses: a handler may respond (e.g. 401 / 413)
// without reading the body.  Each body chunk is therefore delivered with
// a `prialt` that races the body push against the handler's response —
// the connection can never deadlock waiting to push a chunk nobody is
// reading.  Once the handler has responded (or dropped its body_stream),
// the orchestrator keeps draining the body off the wire so the next
// keep-alive request starts at the right byte boundary.

void handle_connection(io::fd_t fd, std::string remote_addr,
                       writer<endpoint> ep_out, size_t read_chunk_size) {
    internal::descr("http/conn");

    chan<request> req_ch;

    endpoint ep;
    ep.requests = std::move(req_ch.r);
    ep.remote_addr = std::move(remote_addr);
    if (!(ep_out << std::move(ep))) {
        io::close(fd);
        return;
    }

    // Non-owning source: the imp reads from the fd but never closes
    // it.  handle_connection retains fd ownership for the explicit
    // io::close at `done:`, or for the WebSocket-upgrade hijack
    // handoff after dropping the source.  Portable across platforms
    // (no ::dup, which Windows doesn't provide for sockets).
    io::source src = io::fd_source_view(fd);

    parse_state state;
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_header_field = on_header_field;
    settings.on_header_value = on_header_value;
    settings.on_headers_complete = on_headers_complete;
    settings.on_body = on_body;
    settings.on_message_complete = on_message_complete;

    llhttp_t parser;
    llhttp_init(&parser, HTTP_REQUEST, &settings);
    parser.data = &state;

    auto req_writer = std::move(req_ch.w);

    // Unconsumed wire bytes carried across reads (and across keep-alive
    // requests: a single read may contain the tail of one request and the
    // head of the next).
    bytes residual;

    for (;;) {  // one iteration per HTTP request (keep-alive)
        state.reset();

        chan<bytes> body_ch;                     // body chunks -> handler
        chan<response> resp_ch;                  // handler's response
        chan<request::hijack_result> hijack_ch;  // protocol-upgrade hijack

        bool delivered = false;     // request handed to the handler yet?
        bool deliver_body = true;   // handler still reading body_stream?
        bool resp_received = false; // handler responded before body end?
        response resp;

        // --- Parse the request, streaming the body as it arrives ---
        for (;;) {
            if (residual.empty()) {
                try {
                    residual = src(read_chunk_size);
                } catch (...) {
                    // EOF (channel_closed) or transport error.  Between
                    // requests this is a normal close; mid-request it is a
                    // truncated request — either way, end the connection.
                    goto done;
                }
            }

            const char* data = reinterpret_cast<const char*>(residual.data());
            size_t len = residual.size();
            auto err = llhttp_execute(&parser, data, len);

            // The parser pauses (rather than consuming everything) at a
            // message boundary: after on_message_complete (HPE_PAUSED), or at
            // the end of an upgrade request's headers (HPE_PAUSED_UPGRADE /
            // HPE_PAUSED_H2_UPGRADE — on_message_complete still fires, then
            // any post-upgrade bytes are handed to the hijacking protocol).
            bool paused = err == HPE_PAUSED ||
                          err == HPE_PAUSED_UPGRADE ||
                          err == HPE_PAUSED_H2_UPGRADE;
            size_t consumed = paused
                ? static_cast<size_t>(llhttp_get_error_pos(&parser) - data)
                : len;
            residual.erase(residual.begin(),
                           residual.begin() +
                               static_cast<std::ptrdiff_t>(consumed));

            if (err != HPE_OK && !paused) {
                response bad{400, {}, {}};
                auto wire = serialize_response(bad);
                (void)io::write(fd, wire.data(), wire.size());
                goto done;
            }

            // Deliver the request as soon as the headers are in — before
            // the body has finished arriving.
            if (state.headers_complete && !delivered) {
                request req;
                req.method = from_llhttp(state.req_method);
                req.url = std::move(state.url);
                req.version_major = state.version_major;
                req.version_minor = state.version_minor;
                req.headers = std::move(state.headers);
                req.keep_alive = state.keep_alive;
                req.body_stream = std::move(body_ch.r);
                req.respond = std::move(resp_ch.w);
                req.hijack  = std::move(hijack_ch.r);
                if (!(req_writer << std::move(req))) goto done;
                delivered = true;
            }

            // Hand this execute's body bytes to the handler, racing the
            // push against an early response so a handler that rejects the
            // request without draining its body cannot deadlock us.
            if (!state.body_stage.empty()) {
                if (deliver_body && !resp_received) {
                    switch (prialt(body_ch.w << std::move(state.body_stage),
                                   resp_ch.r >> resp)) {
                    case 0:  break;                        // chunk delivered
                    case 1:  resp_received = true; break;  // early response
                    case ~0: deliver_body  = false; break; // body_stream dropped
                    case ~1: goto done;                    // response abandoned
                    }
                }
                state.body_stage.clear();
            }

            if (state.message_complete) break;
        }

        // Body fully parsed.  `residual` now holds any bytes read past this
        // message — the next pipelined request, or post-upgrade data for the
        // hijack path.
        bytes leftover = residual;

        // Close the handler's body_stream (EOF) before awaiting the
        // response, so a handler blocked draining the body unblocks and can
        // respond.
        body_ch.w = {};

        if (!resp_received) {
            if (!(resp_ch.r >> resp)) goto done;  // handler dropped, no response
        }

        {
            auto wire = serialize_response(resp);
            if (io::write(fd, wire.data(), wire.size()) < 0) goto done;
        }

        // A 101 Switching Protocols response signals a protocol upgrade
        // (e.g. WebSocket).  Offer the raw fd to the handler via the hijack
        // channel; the handler must already be waiting on req.hijack (it
        // called ws::upgrade, which reads the hijack channel synchronously
        // after sending the 101).
        if (resp.status == 101) {
            // Drop the source so its imp exits before the WebSocket handler
            // starts reading from the fd — the source's read loop and the
            // WS reader cannot safely interleave reads on the same fd.
            src = io::source{};
            hijack_ch.w << request::hijack_result{fd, std::move(leftover)};
            return;  // fd ownership transferred — do NOT close.
        }

        bool ka = state.keep_alive;

        // Always resume — the parser stays paused after message complete
        // even when HPE_OK is returned (all data consumed at the pause
        // point).
        llhttp_resume(&parser);

        if (!ka) goto done;
        // residual carries any pipelined bytes into the next iteration.
    }

done:
    io::close(fd);
}

// --- Accept loop ---
//
// Creates its own TCP listener rather than using net::listen, to avoid
// inheriting net::listen's cancel scope for connection handler imps.

struct listen_result {
    io::fd_t listen_fd;
    uint16_t port;
    std::string local_addr;
};

listen_result create_listener(const std::string& addr, uint16_t port,
                              const net::listen_options& opts) {
    // AF_UNSPEC + AI_PASSIVE only for wildcards (same as net::listen; 🎯T39).
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (addr.empty() || addr == "::" || addr == "0.0.0.0"
        || addr == "0:0:0:0:0:0:0:0") {
        hints.ai_flags = AI_PASSIVE;
    }

    auto result = io::resolve(addr, std::to_string(port), &hints);
    if (!result) {
        throw csp::error(std::string("http listen resolve failed: ") +
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

    struct sockaddr_storage bound {};
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

    return {listen_fd, actual_port, std::move(local)};
}

// --- URL parsing ---

struct parsed_url {
    std::string host;
    uint16_t port = 80;
    std::string path;
};

parsed_url parse_url(const std::string& url) {
    // Expected: http://host[:port][/path]
    const std::string prefix = "http://";
    if (url.size() < prefix.size() ||
        !iequals(url.substr(0, prefix.size()), prefix)) {
        throw csp::error("unsupported URL scheme (only http:// is supported)");
    }

    auto rest = url.substr(prefix.size());
    parsed_url result;

    // Split host[:port] from path.
    auto slash = rest.find('/');
    std::string authority;
    if (slash == std::string::npos) {
        authority = rest;
        result.path = "/";
    } else {
        authority = rest.substr(0, slash);
        result.path = rest.substr(slash);
    }

    // Handle [IPv6]:port
    if (!authority.empty() && authority[0] == '[') {
        auto bracket = authority.find(']');
        if (bracket == std::string::npos) {
            throw csp::error("malformed IPv6 address in URL");
        }
        result.host = authority.substr(1, bracket - 1);
        if (bracket + 1 < authority.size() && authority[bracket + 1] == ':') {
            result.port = static_cast<uint16_t>(
                std::stoul(authority.substr(bracket + 2)));
        }
    } else {
        auto colon = authority.rfind(':');
        if (colon == std::string::npos) {
            result.host = authority;
        } else {
            result.host = authority.substr(0, colon);
            result.port = static_cast<uint16_t>(
                std::stoul(authority.substr(colon + 1)));
        }
    }

    if (result.host.empty()) {
        throw csp::error("empty host in URL");
    }
    return result;
}

// --- Client request serialization ---

bytes serialize_request(method m, const parsed_url& url,
                        const std::vector<std::pair<std::string, std::string>>& hdrs,
                        const bytes& body) {
    std::ostringstream os;
    os << method_name(m) << " " << url.path << " HTTP/1.1\r\n";
    os << "Host: " << url.host;
    if (url.port != 80) os << ":" << url.port;
    os << "\r\n";

    bool has_content_length = false;
    bool has_connection = false;
    for (auto& [k, v] : hdrs) {
        os << k << ": " << v << "\r\n";
        if (iequals(k, "Content-Length")) has_content_length = true;
        if (iequals(k, "Connection")) has_connection = true;
    }
    if (!has_content_length && !body.empty()) {
        os << "Content-Length: " << body.size() << "\r\n";
    }
    if (!has_connection) {
        os << "Connection: close\r\n";
    }
    os << "\r\n";

    auto head = os.str();
    bytes out;
    out.reserve(head.size() + body.size());
    out.insert(out.end(), head.begin(), head.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// --- Client response parser state ---

struct resp_parse_state {
    int status_code = 0;
    std::string header_field;
    std::string header_value;
    std::vector<std::pair<std::string, std::string>> headers;
    bytes body;
    bool in_value = false;
    bool message_complete = false;

    void flush_header() {
        if (in_value) {
            headers.emplace_back(std::move(header_field),
                                 std::move(header_value));
            header_field.clear();
            header_value.clear();
            in_value = false;
        }
    }
};

int resp_on_status(llhttp_t*, const char*, size_t) {
    return 0;
}

int resp_on_header_field(llhttp_t* p, const char* at, size_t len) {
    auto* st = static_cast<resp_parse_state*>(p->data);
    st->flush_header();
    st->header_field.append(at, len);
    return 0;
}

int resp_on_header_value(llhttp_t* p, const char* at, size_t len) {
    auto* st = static_cast<resp_parse_state*>(p->data);
    st->in_value = true;
    st->header_value.append(at, len);
    return 0;
}

int resp_on_headers_complete(llhttp_t* p) {
    auto* st = static_cast<resp_parse_state*>(p->data);
    st->flush_header();
    st->status_code = static_cast<int>(p->status_code);
    return 0;
}

int resp_on_body(llhttp_t* p, const char* at, size_t len) {
    auto* st = static_cast<resp_parse_state*>(p->data);
    st->body.insert(st->body.end(),
                    reinterpret_cast<const uint8_t*>(at),
                    reinterpret_cast<const uint8_t*>(at) + len);
    return 0;
}

int resp_on_message_complete(llhttp_t* p) {
    static_cast<resp_parse_state*>(p->data)->message_complete = true;
    return 0;
}

} // anonymous namespace

// --- request helpers ---

std::string request::header(const std::string& name) const {
    for (auto& [k, v] : headers) {
        if (iequals(k, name)) return v;
    }
    return {};
}

int64_t request::content_length() const {
    auto v = header("Content-Length");
    if (v.empty()) return -1;
    return std::stoll(v);
}

const bytes& request::drain() {
    bytes chunk;
    while (body_stream >> chunk) {
        body.insert(body.end(), chunk.begin(), chunk.end());
    }
    return body;
}

// --- serve ---

server serve(uint16_t port, serve_options opts) {
    return serve("::", port, opts);
}

server serve(const std::string& addr, uint16_t port, serve_options opts) {
    auto lr = create_listener(addr, port, opts.listen);

    // The accept loop uses a sentinel pattern for clean shutdown.
    // When all endpoint readers die, a sentinel imp self-connects to
    // the listen socket to unblock io::accept, then sets a stop flag.
    // No cancel scope is used, so connection handler imps are free of
    // cancel-aware I/O interference.

    auto endpoints = spawn_producer<endpoint>(
        [listen_fd = lr.listen_fd, port = lr.port,
         read_chunk_size = opts.read_chunk_size](writer<endpoint> out) {
            internal::descr("http/serve");

            auto stop = std::make_shared<std::atomic<bool>>(false);

            // Sentinel: self-connect to unblock accept when reader dies.
            auto out_copy = out.copy();
            auto stop_flag = stop;
            csp::spawn([out_copy = std::move(out_copy),
                         stop_flag, port] {
                internal::descr("http/serve/sentinel");
                prialt(~out_copy);
                stop_flag->store(true, std::memory_order_release);

                // Self-connect to unblock the accept loop.
#ifdef _WIN32
                SOCKET raw = ::socket(AF_INET6, SOCK_STREAM, 0);
                if (raw != INVALID_SOCKET) {
                    struct sockaddr_in6 addr {};
                    addr.sin6_family = AF_INET6;
                    addr.sin6_port = htons(port);
                    addr.sin6_addr = in6addr_loopback;
                    ::connect(raw, reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr));
                    closesocket(raw);
                }
#else
                int raw = ::socket(AF_INET6, SOCK_STREAM, 0);
                if (raw >= 0) {
                    struct sockaddr_in6 addr {};
                    addr.sin6_family = AF_INET6;
                    addr.sin6_port = htons(port);
                    addr.sin6_addr = in6addr_loopback;
                    ::connect(raw, reinterpret_cast<sockaddr*>(&addr),
                              sizeof(addr));
                    ::close(raw);
                }
#endif
            });

            for (;;) {
                struct sockaddr_storage client_addr {};
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
                            read_chunk_size]() mutable {
                    handle_connection(fd, std::move(r), std::move(o),
                                      read_chunk_size);
                });
            }
            io::close(listen_fd);
        });

    return {std::move(endpoints), lr.port, std::move(lr.local_addr)};
}

// --- fetch ---

response fetch(
    method m, const std::string& url,
    std::vector<std::pair<std::string, std::string>> headers,
    bytes body,
    fetch_options opts) {

    auto parsed = parse_url(url);
    auto wire = serialize_request(m, parsed, headers, body);

    // Connect via net::dial, then do direct I/O on the fd.
    // We use the raw fd rather than the connection's channels to avoid
    // spawning reader/writer imps for a single request-response cycle.
    auto conn = net::dial(parsed.host, parsed.port);

    // Write the request via the connection's output channel.
    // Then close the writer to signal we're done sending.
    conn.output << std::move(wire);

    // Read the full response via the connection's input channel.
    resp_parse_state state;
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_status = resp_on_status;
    settings.on_header_field = resp_on_header_field;
    settings.on_header_value = resp_on_header_value;
    settings.on_headers_complete = resp_on_headers_complete;
    settings.on_body = resp_on_body;
    settings.on_message_complete = resp_on_message_complete;

    llhttp_t parser;
    llhttp_init(&parser, HTTP_RESPONSE, &settings);
    parser.data = &state;

    // Read the response through the pull-based source (🎯T17).
    bytes chunk;
    while (!state.message_complete) {
        auto reply_r = csp::io::call_source(conn.source, opts.read_chunk_size);
        if (!(reply_r >> chunk)) break;
        auto err = llhttp_execute(
            &parser,
            reinterpret_cast<const char*>(chunk.data()),
            chunk.size());

        if (state.message_complete) break;

        if (err != HPE_OK) {
            throw csp::error(
                std::string("HTTP response parse error: ") +
                llhttp_errno_name(err));
        }
    }

    if (!state.message_complete) {
        // Connection closed before a complete response.
        // If we got headers, return what we have (server may have
        // signaled end-of-body via connection close).
        llhttp_finish(&parser);
        if (!state.message_complete && state.status_code == 0) {
            throw csp::error("HTTP response incomplete: connection closed");
        }
    }

    return response{
        state.status_code,
        std::move(state.headers),
        std::move(state.body)};
}

// Streaming-body overload (🎯T17).  Writes headers, then pulls
// `body_length` bytes from the source and writes them to the
// connection.  Response read is identical to the buffered overload.
response fetch(
    method m, const std::string& url,
    std::vector<std::pair<std::string, std::string>> headers,
    io::source body,
    size_t body_length,
    fetch_options opts) {

    auto parsed = parse_url(url);

    // Build the request head with an explicit Content-Length.
    std::ostringstream os;
    os << method_name(m) << " " << parsed.path << " HTTP/1.1\r\n";
    os << "Host: " << parsed.host;
    if (parsed.port != 80) os << ":" << parsed.port;
    os << "\r\n";

    bool has_content_length = false;
    bool has_connection = false;
    for (auto& [k, v] : headers) {
        os << k << ": " << v << "\r\n";
        if (iequals(k, "Content-Length")) has_content_length = true;
        if (iequals(k, "Connection"))     has_connection = true;
    }
    if (!has_content_length) {
        os << "Content-Length: " << body_length << "\r\n";
    }
    if (!has_connection) {
        os << "Connection: close\r\n";
    }
    os << "\r\n";

    auto head = os.str();
    bytes head_bytes(head.begin(), head.end());

    auto conn = net::dial(parsed.host, parsed.port);
    conn.output << std::move(head_bytes);

    // Stream the body in chunks from the source.
    size_t remaining = body_length;
    while (remaining > 0) {
        size_t want = std::min(remaining, opts.read_chunk_size);
        auto reply_r = csp::io::call_source(body, want);
        bytes chunk;
        if (!(reply_r >> chunk)) {
            throw csp::error("http::fetch: body source EOF before "
                             "body_length bytes sent");
        }
        size_t got = chunk.size();
        if (!(conn.output << std::move(chunk))) {
            throw csp::error("http::fetch: connection write failed during body stream");
        }
        remaining -= got;
    }

    // Read the response (identical to the buffered-body overload).
    resp_parse_state state;
    llhttp_settings_t settings;
    llhttp_settings_init(&settings);
    settings.on_status            = resp_on_status;
    settings.on_header_field      = resp_on_header_field;
    settings.on_header_value      = resp_on_header_value;
    settings.on_headers_complete  = resp_on_headers_complete;
    settings.on_body              = resp_on_body;
    settings.on_message_complete  = resp_on_message_complete;

    llhttp_t parser;
    llhttp_init(&parser, HTTP_RESPONSE, &settings);
    parser.data = &state;

    bytes chunk;
    while (!state.message_complete) {
        auto reply_r = csp::io::call_source(conn.source, opts.read_chunk_size);
        if (!(reply_r >> chunk)) break;
        auto err = llhttp_execute(
            &parser,
            reinterpret_cast<const char*>(chunk.data()),
            chunk.size());
        if (state.message_complete) break;
        if (err != HPE_OK) {
            throw csp::error(
                std::string("HTTP response parse error: ") +
                llhttp_errno_name(err));
        }
    }

    if (!state.message_complete) {
        llhttp_finish(&parser);
        if (!state.message_complete && state.status_code == 0) {
            throw csp::error("HTTP response incomplete: connection closed");
        }
    }

    return response{
        state.status_code,
        std::move(state.headers),
        std::move(state.body)};
}

// --- Factory for csp::net::serve (🎯T23.1) ---
//
// The `apply()` lambda lives in this TU (csp_http.cpp in the dist drop-in),
// so referencing `csp::http::enable()` from user code pulls the entire TU
// — including its llhttp_* references — into the link. The front-door TU
// (csp.cpp) reaches enable() only through the function-pointer-shaped
// `protocol_option`, which leaves Rule 5 (no protocol symbols in the front
// door) intact.

csp::net::protocol_option enable(serve_options opts) {
    auto* cfg = new serve_options(std::move(opts));
    return csp::net::protocol_option{
        .config = cfg,
        .apply = [](csp::net::apply_context& ctx, void* config) {
            auto* c = static_cast<serve_options*>(config);
            // Start a real HTTP/1.1 server and hand its handle to the
            // unified net::server. csp::http::server is move-only (its
            // endpoints reader can't be copied), so we wrap it in a
            // shared_ptr to fit std::any's copyable requirement. Callers
            // retrieve the typed handle via
            // std::any_cast<std::shared_ptr<csp::http::server>>(s.protocol_servers[0]).
            ctx.out->protocol_servers.emplace_back(
                std::make_shared<csp::http::server>(
                    csp::http::serve(ctx.port, *c)));
        },
        .destroy = [](void* config) {
            delete static_cast<serve_options*>(config);
        },
    };
}

} // namespace csp::http
