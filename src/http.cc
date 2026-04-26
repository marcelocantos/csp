// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <csp/http.h>
#include <csp/cancel.h>
#include <csp/internal/signal.h>

#include <llhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
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
    bytes body;
    bool in_value = false;

    llhttp_method_t req_method = HTTP_GET;
    uint8_t version_major = 1;
    uint8_t version_minor = 1;
    bool keep_alive = true;

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
        body.clear();
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
    return 0;
}

int on_body(llhttp_t* p, const char* at, size_t len) {
    auto* st = static_cast<parse_state*>(p->data);
    st->body.insert(st->body.end(),
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

void handle_connection(io::fd_t fd, std::string remote_addr,
                       writer<endpoint> ep_out) {
    internal::descr("http/conn");

    chan<request> req_ch;

    endpoint ep;
    ep.requests = std::move(req_ch.r);
    ep.remote_addr = std::move(remote_addr);
    if (!(ep_out << std::move(ep))) {
        io::close(fd);
        return;
    }

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

    // Read raw bytes directly from the socket.
    constexpr size_t buf_size = 4096;
    uint8_t buf[buf_size];
    for (;;) {
        ssize_t n = io::read(fd, buf, buf_size);
        if (n <= 0) break;

        const char* data = reinterpret_cast<const char*>(buf);
        size_t len = static_cast<size_t>(n);

        while (len > 0) {
            state.message_complete = false;
            auto err = llhttp_execute(&parser, data, len);

            if (state.message_complete) {
                // Bytes that were consumed by the parser beyond the end
                // of this message (may be non-zero when HPE_PAUSED).
                size_t consumed = 0;
                if (err == HPE_PAUSED) {
                    consumed = static_cast<size_t>(
                        llhttp_get_error_pos(&parser) - data);
                } else {
                    consumed = len;
                }

                // Collect any leftover bytes the parser has already buffered
                // but not yet returned (bytes parsed past this message).
                bytes leftover;
                leftover.insert(leftover.end(),
                                reinterpret_cast<const uint8_t*>(data + consumed),
                                reinterpret_cast<const uint8_t*>(data + len));

                chan<response> resp_ch;
                // Sync hijack channel: write blocks until handler reads
                // or handler drops the reader (no hijack).
                chan<request::hijack_result> hijack_ch;

                request req;
                req.method = from_llhttp(state.req_method);
                req.url = std::move(state.url);
                req.version_major = state.version_major;
                req.version_minor = state.version_minor;
                req.headers = std::move(state.headers);
                req.body = std::move(state.body);
                req.keep_alive = state.keep_alive;
                req.respond = std::move(resp_ch.w);
                req.hijack  = std::move(hijack_ch.r);

                if (!(req_writer << std::move(req))) goto done;

                response resp;
                if (resp_ch.r >> resp) {
                    auto wire = serialize_response(resp);
                    if (io::write(fd, wire.data(), wire.size()) < 0)
                        goto done;

                    // A 101 Switching Protocols response signals a protocol
                    // upgrade (e.g. WebSocket).  Offer the raw fd to the
                    // handler via the hijack channel; the handler must be
                    // waiting on req.hijack.  This write is blocking because
                    // the WebSocket handler must have already started reading
                    // from req.hijack (it called ws::upgrade which reads the
                    // hijack channel synchronously after sending the 101).
                    if (resp.status == 101) {
                        hijack_ch.w << request::hijack_result{fd, std::move(leftover)};
                        // fd ownership transferred — do NOT close.
                        return;
                    }
                }

                bool ka = state.keep_alive;

                // Always resume — the parser stays paused even if
                // HPE_OK is returned (all data consumed at the pause
                // point).
                llhttp_resume(&parser);
                state.reset();

                if (!ka) goto done;

                data += consumed;
                len  -= consumed;
                if (len == 0) break;

            } else if (err != HPE_OK) {
                response bad{400, {}, {}};
                auto wire = serialize_response(bad);
                io::write(fd, wire.data(), wire.size());
                goto done;

            } else {
                break;
            }
        }
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
    struct addrinfo hints {};
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

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
        [listen_fd = lr.listen_fd, port = lr.port](writer<endpoint> out) {
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
                            o = std::move(out_copy2)]() mutable {
                    handle_connection(fd, std::move(r), std::move(o));
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

    bytes chunk;
    while (!state.message_complete && (conn.input >> chunk)) {
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

} // namespace csp::http
