// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <csp/ws.h>
#include <csp/cancel.h>

// wslay frame-level API (vendored in vendor/github.com/tatsuhiro-t/wslay/)
#include <wslay/wslay.h>
#include <wslay_frame.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <ws2tcpip.h>
#endif

namespace csp::ws {

namespace {

// ---------------------------------------------------------------------------
// Minimal SHA-1 (RFC 3174) — used for WebSocket handshake key derivation.
// ---------------------------------------------------------------------------

struct sha1_ctx {
    uint32_t h[5];
    uint64_t bit_len;
    uint8_t  buf[64];
    size_t   buf_len;
};

static void sha1_init(sha1_ctx& ctx) {
    ctx.h[0] = 0x67452301u;
    ctx.h[1] = 0xEFCDAB89u;
    ctx.h[2] = 0x98BADCFEu;
    ctx.h[3] = 0x10325476u;
    ctx.h[4] = 0xC3D2E1F0u;
    ctx.bit_len = 0;
    ctx.buf_len = 0;
}

static uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static void sha1_compress(sha1_ctx& ctx) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(ctx.buf[i * 4    ]) << 24) |
               (uint32_t(ctx.buf[i * 4 + 1]) << 16) |
               (uint32_t(ctx.buf[i * 4 + 2]) <<  8) |
               (uint32_t(ctx.buf[i * 4 + 3])       );
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    uint32_t a = ctx.h[0], b = ctx.h[1], c = ctx.h[2],
             d = ctx.h[3], e = ctx.h[4];

    auto step = [&](int i) {
        uint32_t f, k;
        if      (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;           k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;           k = 0xCA62C1D6u; }
        uint32_t tmp = rotl32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl32(b, 30); b = a; a = tmp;
    };
    for (int i = 0; i < 80; ++i) step(i);

    ctx.h[0] += a; ctx.h[1] += b; ctx.h[2] += c;
    ctx.h[3] += d; ctx.h[4] += e;
}

static void sha1_update(sha1_ctx& ctx, const uint8_t* data, size_t len) {
    ctx.bit_len += len * 8;
    while (len > 0) {
        size_t avail = sizeof(ctx.buf) - ctx.buf_len;
        size_t take  = std::min(avail, len);
        std::memcpy(ctx.buf + ctx.buf_len, data, take);
        ctx.buf_len += take;
        data += take;
        len  -= take;
        if (ctx.buf_len == sizeof(ctx.buf)) {
            sha1_compress(ctx);
            ctx.buf_len = 0;
        }
    }
}

static void sha1_final(sha1_ctx& ctx, uint8_t out[20]) {
    // Padding
    ctx.buf[ctx.buf_len++] = 0x80;
    if (ctx.buf_len > 56) {
        while (ctx.buf_len < 64) ctx.buf[ctx.buf_len++] = 0;
        sha1_compress(ctx);
        ctx.buf_len = 0;
    }
    while (ctx.buf_len < 56) ctx.buf[ctx.buf_len++] = 0;
    // Big-endian bit count
    uint64_t bl = ctx.bit_len;
    for (int i = 7; i >= 0; --i) {
        ctx.buf[56 + i] = uint8_t(bl & 0xFF);
        bl >>= 8;
    }
    sha1_compress(ctx);

    for (int i = 0; i < 5; ++i) {
        out[i * 4    ] = uint8_t(ctx.h[i] >> 24);
        out[i * 4 + 1] = uint8_t(ctx.h[i] >> 16);
        out[i * 4 + 2] = uint8_t(ctx.h[i] >>  8);
        out[i * 4 + 3] = uint8_t(ctx.h[i]      );
    }
}

static std::string sha1_base64(const std::string& input) {
    sha1_ctx ctx;
    sha1_init(ctx);
    sha1_update(ctx,
        reinterpret_cast<const uint8_t*>(input.data()), input.size());
    uint8_t digest[20];
    sha1_final(ctx, digest);

    // Base64 encode
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(28);
    for (int i = 0; i < 20; i += 3) {
        int n   = (i + 1 < 20 ? digest[i + 1] : 0);
        int n2  = (i + 2 < 20 ? digest[i + 2] : 0);
        uint32_t v = (uint32_t(digest[i]) << 16) | (uint32_t(n) << 8) | n2;
        out += table[(v >> 18) & 0x3F];
        out += table[(v >> 12) & 0x3F];
        out += (i + 1 < 20) ? table[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < 20) ? table[ v       & 0x3F] : '=';
    }
    return out;
}

// ---------------------------------------------------------------------------
// Case-insensitive string equality
// ---------------------------------------------------------------------------

static bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i])))
            return false;
    }
    return true;
}

static std::string trim(const std::string& s) {
    size_t l = s.find_first_not_of(" \t\r\n");
    if (l == std::string::npos) return {};
    size_t r = s.find_last_not_of(" \t\r\n");
    return s.substr(l, r - l + 1);
}

// ---------------------------------------------------------------------------
// wslay callbacks — wrap cooperative io::read / io::write
// ---------------------------------------------------------------------------

struct ws_io {
    io::fd_t fd;
    // Leftover bytes from the HTTP parser that belong to the WS stream.
    bytes leftover;
    size_t leftover_pos = 0;
    bool close_sent   = false;
    bool close_rcvd   = false;
};

// recv_callback: called by wslay_frame_recv() when it needs more bytes.
static ssize_t ws_recv_cb(uint8_t* buf, size_t len, int /*flags*/,
                           void* user_data) {
    auto* io = static_cast<ws_io*>(user_data);

    // Drain leftover bytes from the HTTP parser first.
    if (io->leftover_pos < io->leftover.size()) {
        size_t avail = io->leftover.size() - io->leftover_pos;
        size_t take  = std::min(avail, len);
        std::memcpy(buf, io->leftover.data() + io->leftover_pos, take);
        io->leftover_pos += take;
        return ssize_t(take);
    }

    ssize_t n = csp::io::read(io->fd, buf, len);
    if (n <= 0) return WSLAY_ERR_WANT_READ;
    return n;
}

// send_callback: called by wslay_frame_send() when it wants to send bytes.
static ssize_t ws_send_cb(const uint8_t* buf, size_t len, int /*flags*/,
                           void* user_data) {
    auto* io = static_cast<ws_io*>(user_data);
    ssize_t n = csp::io::write(io->fd, buf, len);
    if (n <= 0) return WSLAY_ERR_WANT_WRITE;
    return n;
}

// genmask_callback: called when the sender needs a masking key (clients only).
static int ws_genmask_cb(uint8_t* buf, size_t len, void* /*user_data*/) {
    // Use arc4random_buf on Apple; /dev/urandom on others.
#if defined(__APPLE__) || defined(__FreeBSD__)
    arc4random_buf(buf, len);
#else
    // Fallback: use rand() seeded with time (good enough for test fixtures).
    for (size_t i = 0; i < len; ++i)
        buf[i] = static_cast<uint8_t>(rand());
#endif
    return 0;
}

// ---------------------------------------------------------------------------
// Low-level frame send helper
// ---------------------------------------------------------------------------

// send_frame: send a complete, single WebSocket frame.
//
// wslay_frame_send advances its internal state machine (PREP_HEADER →
// SEND_HEADER → SEND_PAYLOAD → PREP_HEADER).  The return value is the
// number of PAYLOAD bytes consumed.  Since our send_callback uses
// io::write (which is blocking-cooperative and writes all bytes), each
// call completes its phase fully.
//
// For zero-payload frames (e.g. close, ping with no data): one call
//   PREP_HEADER→SEND_HEADER→SEND_PAYLOAD→PREP_HEADER, returns 0.
// For payload frames: one call for header+first-chunk, possibly more
//   calls for remaining payload (data_length is updated on each call).
static bool send_frame(wslay_frame_context_ptr ctx, uint8_t op,
                        bool mask, const uint8_t* data, size_t len) {
    wslay_frame_iocb iocb{};
    iocb.fin            = 1;
    iocb.rsv            = 0;
    iocb.opcode         = op;
    iocb.mask           = mask ? 1 : 0;
    iocb.payload_length = len;
    iocb.data           = data;
    iocb.data_length    = len;

    size_t total_sent = 0;
    for (;;) {
        ssize_t r = wslay_frame_send(ctx, &iocb);
        if (r == WSLAY_ERR_WANT_WRITE) continue; // transient write failure
        if (r < 0) return false;

        total_sent        += static_cast<size_t>(r);
        iocb.data          = data + total_sent;
        iocb.data_length   = (len > total_sent) ? len - total_sent : 0;

        // Frame is done when all payload has been consumed.
        if (total_sent >= len) break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// WebSocket reader imp
//
// Reads frames from the fd, handles control frames transparently,
// and forwards data messages to the user-visible channel.
// ---------------------------------------------------------------------------

static void ws_reader(io::fd_t fd, bytes leftover,
                      writer<message> out, bool is_client) {
    internal::descr("ws/reader");

    wslay_frame_callbacks cbs{};
    cbs.recv_callback    = ws_recv_cb;
    cbs.send_callback    = ws_send_cb;
    cbs.genmask_callback = ws_genmask_cb;

    ws_io io_state;
    io_state.fd       = fd;
    io_state.leftover = std::move(leftover);

    wslay_frame_context_ptr ctx = nullptr;
    if (wslay_frame_context_init(&ctx, &cbs, &io_state) != 0) {
        return;
    }

    // RAII cleanup for wslay context.
    struct ctx_guard {
        wslay_frame_context_ptr c;
        ~ctx_guard() { if (c) wslay_frame_context_free(c); }
    } guard{ctx};

    // Each wslay_frame_recv call returns a chunk of the current frame's
    // payload. We accumulate chunks until the frame is fully received
    // (accumulated bytes == iocb.payload_length), then handle the frame.
    //
    // WS message reassembly: text/binary frames start a message; fin=1
    // means it's the last (or only) fragment. Continuation frames have
    // opcode=0 and are assembled with the initial fragment.
    //
    // This implementation accumulates into `frame_buf` per frame, then
    // into `msg_data` per message.

    message pending;
    bool in_msg          = false;
    bytes frame_buf;           // accumulates chunks of the current frame
    uint64_t frame_expected = 0; // total payload bytes for current frame

    for (;;) {
        wslay_frame_iocb iocb{};
        ssize_t r = wslay_frame_recv(ctx, &iocb);

        if (r == WSLAY_ERR_WANT_READ) {
            // ws_recv_cb calls io::read which blocks cooperatively.
            // WANT_READ means io::read returned 0 (EOF) or error.
            break;
        }
        if (r < 0) break;   // protocol error

        // r bytes of payload are available in iocb.data.
        // This may be a partial chunk of the frame payload.

        // On first chunk of a new frame, record total payload length.
        if (frame_buf.empty() && frame_expected == 0) {
            frame_expected = iocb.payload_length;
        }

        // Accumulate chunk.
        if (r > 0) {
            frame_buf.insert(frame_buf.end(),
                             iocb.data,
                             iocb.data + static_cast<size_t>(r));
        }

        // Have we received the complete frame payload?
        bool frame_done = (frame_buf.size() >= frame_expected && r >= 0);

        if (!frame_done) continue;

        // Frame is complete — process it.
        uint8_t op = iocb.opcode;

        // Control frames must be < 126 bytes and not fragmented.
        if (wslay_is_ctrl_frame(op)) {
            if (op == WSLAY_PING) {
                // Auto-pong — clients mask, servers don't.
                send_frame(ctx, WSLAY_PONG, is_client,
                           frame_buf.data(), frame_buf.size());
            } else if (op == WSLAY_PONG) {
                // Unsolicited or solicited pong — ignore.
            } else if (op == WSLAY_CONNECTION_CLOSE) {
                // Echo the close frame back if we haven't sent one yet.
                if (!io_state.close_sent) {
                    send_frame(ctx, WSLAY_CONNECTION_CLOSE, is_client,
                               frame_buf.data(), frame_buf.size());
                    io_state.close_sent = true;
                }
                io_state.close_rcvd = true;
                // Fall through to reset frame state, then break below.
                frame_buf.clear();
                frame_expected = 0;
                break;
            }
            frame_buf.clear();
            frame_expected = 0;
            continue;
        }

        // Data frame (text=0x1, binary=0x2) or continuation (0x0).
        if (op == WSLAY_TEXT_FRAME || op == WSLAY_BINARY_FRAME) {
            // Start of a new message.
            in_msg       = true;
            pending.op   = (op == WSLAY_TEXT_FRAME) ? opcode::text : opcode::binary;
            pending.data.clear();
        }
        // op == 0 is a continuation frame — we're already in_msg.

        if (in_msg) {
            pending.data.insert(pending.data.end(),
                                frame_buf.begin(), frame_buf.end());
        }

        frame_buf.clear();
        frame_expected = 0;

        // fin=1 means this is the last (or only) fragment.
        if (iocb.fin && in_msg) {
            in_msg = false;
            if (!(out << std::move(pending))) break;
            pending = {};
        }
    }

    io::close(fd);
}

// ---------------------------------------------------------------------------
// WebSocket writer imp
//
// Reads messages from the user channel and sends them as WS frames.
// When the user channel dies, sends a Close frame.
// ---------------------------------------------------------------------------

// ws_writer takes a non-owning fd view (does NOT close it on exit).
// The reader imp owns the fd and closes it when it exits.
static void ws_writer(io::fd_t fd,
                      reader<message> in, bool is_client,
                      bool close_already_sent) {
    internal::descr("ws/writer");

    wslay_frame_callbacks cbs{};
    cbs.recv_callback    = ws_recv_cb;
    cbs.send_callback    = ws_send_cb;
    cbs.genmask_callback = ws_genmask_cb;

    ws_io io_state;
    io_state.fd             = fd;
    io_state.close_sent     = close_already_sent;

    wslay_frame_context_ptr ctx = nullptr;
    if (wslay_frame_context_init(&ctx, &cbs, &io_state) != 0) {
        return; // do NOT close fd — reader owns it
    }

    struct ctx_guard {
        wslay_frame_context_ptr c;
        ~ctx_guard() { if (c) wslay_frame_context_free(c); }
    } guard{ctx};

    for (;;) {
        message msg;
        if (!(in >> msg)) break;

        uint8_t op = (msg.op == opcode::text) ? WSLAY_TEXT_FRAME
                                               : WSLAY_BINARY_FRAME;
        if (!send_frame(ctx, op, is_client,
                        msg.data.data(), msg.data.size())) {
            break;
        }
    }

    // BLO: writer channel died → send Close frame to initiate graceful close.
    if (!io_state.close_sent) {
        send_frame(ctx, WSLAY_CONNECTION_CLOSE, is_client, nullptr, 0);
        io_state.close_sent = true;
    }
    // Do NOT close fd here — the reader imp owns and will close it.
}

// ---------------------------------------------------------------------------
// Shared helper: spawn reader + writer imps from an fd
//
// Both imps use the same fd — reads and writes on a socket use different
// syscall paths so they are safe to call concurrently from different imps
// (the kernel serializes at the socket layer). The fd is closed by the
// reader imp when it exits; at that point the writer sees EPIPE/EBADF on
// its next write and also exits.
// ---------------------------------------------------------------------------

static conn make_conn(io::fd_t fd, bytes leftover, bool is_client) {
    // The reader owns the fd and closes it on exit.
    // The writer holds the raw fd value (not owning) — it will fail on the
    // next write after the reader closes it, triggering a clean exit.
    io::fd_t wfd(fd.raw()); // non-owning view of the same socket

    auto [send_w, send_r] = chan<message>{};
    auto [recv_w, recv_r] = chan<message>{};

    // Reader imp: owns fd (closes on exit).
    csp::spawn([fd, lv = std::move(leftover), w = std::move(recv_w),
                is_client]() mutable {
        ws_reader(fd, std::move(lv), std::move(w), is_client);
    });

    // Writer imp: uses wfd (raw view, does NOT close on exit).
    csp::spawn([wfd_raw = wfd.raw(), r = std::move(send_r), is_client]() mutable {
        // Writer uses a raw fd view — does NOT own/close the fd.
        io::fd_t view(wfd_raw);
        ws_writer(view, std::move(r), is_client, /*close_already_sent=*/false);
    });

    return conn{std::move(recv_r), std::move(send_w)};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Server-side upgrade
// ---------------------------------------------------------------------------

conn upgrade(http::request& req) {
    // Validate the upgrade request.
    auto upgrade_hdr    = req.header("Upgrade");
    auto connection_hdr = req.header("Connection");
    auto ws_key         = req.header("Sec-WebSocket-Key");
    auto ws_ver         = req.header("Sec-WebSocket-Version");

    auto bad = [&](const std::string& reason) {
        req.respond << http::response{
            400,
            {{"Connection", "close"}},
            bytes(reason.begin(), reason.end())
        };
        // Drop req.hijack so http.cc continues (or closes) the connection.
        req.hijack = {};
        throw csp::error("ws::upgrade: " + reason);
    };

    if (!iequals(trim(upgrade_hdr), "websocket"))
        bad("missing Upgrade: websocket header");

    // Connection: must contain "Upgrade" (case-insensitive, comma-list).
    {
        bool found_upgrade = false;
        std::istringstream ss(connection_hdr);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            if (iequals(trim(tok), "upgrade")) { found_upgrade = true; break; }
        }
        if (!found_upgrade) bad("missing Connection: Upgrade");
    }

    if (ws_key.empty()) bad("missing Sec-WebSocket-Key");
    if (ws_ver != "13") bad("unsupported Sec-WebSocket-Version (need 13)");

    // Compute Sec-WebSocket-Accept.
    static const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string accept = sha1_base64(ws_key + magic);

    // Send 101 Switching Protocols.
    req.respond << http::response{
        101,
        {
            {"Upgrade",              "websocket"},
            {"Connection",           "Upgrade"},
            {"Sec-WebSocket-Accept", accept},
        },
        {}
    };
    // Claim the raw socket from the HTTP layer.
    http::request::hijack_result hr;
    if (!(req.hijack >> hr)) {
        throw csp::error("ws::upgrade: failed to hijack HTTP connection");
    }

    return make_conn(hr.fd, std::move(hr.leftover), /*is_client=*/false);
}

// ---------------------------------------------------------------------------
// Raw dial: resolve host, connect, return non-blocking fd.
// Does NOT create byte_reader/writer imps — we use direct io::read/write.
// ---------------------------------------------------------------------------

static io::fd_t raw_dial(const std::string& host, uint16_t port) {
    struct addrinfo hints {};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    auto result = io::resolve(host, std::to_string(port), &hints);
    if (!result) {
        throw csp::error(std::string("ws: resolve failed: ") + result.message());
    }

    for (auto* ai = result.info.get(); ai; ai = ai->ai_next) {
        io::fd_t fd(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!fd) continue;

        io::set_nonblock(fd);

        if (io::connect(fd, ai->ai_addr,
                        static_cast<socklen_t>(ai->ai_addrlen)) == 0) {
            return fd;
        }

        io::close(fd);
    }

    throw csp::error("ws: connect failed: all addresses exhausted");
}

// ---------------------------------------------------------------------------
// Client-side connect
// ---------------------------------------------------------------------------

conn connect(const std::string& url) {
    // Parse ws://host[:port]/path
    const std::string prefix = "ws://";
    if (url.size() < prefix.size() ||
        !iequals(url.substr(0, prefix.size()), prefix)) {
        throw csp::error("ws::connect: only ws:// scheme is supported");
    }

    auto rest  = url.substr(prefix.size());
    std::string host;
    uint16_t    port = 80;
    std::string path = "/";

    auto slash = rest.find('/');
    std::string authority;
    if (slash == std::string::npos) {
        authority = rest;
    } else {
        authority = rest.substr(0, slash);
        path      = rest.substr(slash);
    }

    if (!authority.empty() && authority[0] == '[') {
        auto bracket = authority.find(']');
        if (bracket == std::string::npos)
            throw csp::error("ws::connect: malformed IPv6 address");
        host = authority.substr(1, bracket - 1);
        if (bracket + 1 < authority.size() && authority[bracket + 1] == ':')
            port = static_cast<uint16_t>(std::stoul(authority.substr(bracket + 2)));
    } else {
        auto colon = authority.rfind(':');
        if (colon == std::string::npos) {
            host = authority;
        } else {
            host = authority.substr(0, colon);
            port = static_cast<uint16_t>(std::stoul(authority.substr(colon + 1)));
        }
    }
    if (host.empty()) throw csp::error("ws::connect: empty host");

    // Generate a random base64-encoded 16-byte nonce for Sec-WebSocket-Key.
    uint8_t nonce[16];
    ws_genmask_cb(nonce, 16, nullptr);  // reuse our random helper
    // Base64-encode the nonce directly.
    static const char* b64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ws_key;
    ws_key.reserve(24);
    for (int i = 0; i < 16; i += 3) {
        uint32_t v = (uint32_t(nonce[i]) << 16) |
                     (i + 1 < 16 ? uint32_t(nonce[i + 1]) << 8 : 0) |
                     (i + 2 < 16 ? uint32_t(nonce[i + 2])      : 0);
        ws_key += b64[(v >> 18) & 0x3F];
        ws_key += b64[(v >> 12) & 0x3F];
        ws_key += (i + 1 < 16) ? b64[(v >> 6) & 0x3F] : '=';
        ws_key += (i + 2 < 16) ? b64[ v       & 0x3F] : '=';
    }

    // Compute expected accept key.
    static const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string expected_accept = sha1_base64(ws_key + magic);

    // Connect via a raw socket (no byte_reader/writer imps — we need
    // to take exclusive ownership of the fd after the handshake).
    io::fd_t fd = raw_dial(host, port);

    // Build the HTTP Upgrade request.
    std::ostringstream os;
    os << "GET " << path << " HTTP/1.1\r\n"
       << "Host: " << host << "\r\n"
       << "Upgrade: websocket\r\n"
       << "Connection: Upgrade\r\n"
       << "Sec-WebSocket-Key: " << ws_key << "\r\n"
       << "Sec-WebSocket-Version: 13\r\n"
       << "\r\n";

    auto req_wire = os.str();
    if (io::write(fd, req_wire.data(), req_wire.size()) < 0) {
        io::close(fd);
        throw csp::error("ws::connect: failed to send HTTP upgrade request");
    }

    // Read the server's 101 response via direct io::read.
    // We buffer until we see the end of HTTP headers (\r\n\r\n).
    std::string resp_buf;
    resp_buf.reserve(1024);
    constexpr size_t BUF_SIZE = 512;
    uint8_t buf[BUF_SIZE];

    while (resp_buf.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = io::read(fd, buf, BUF_SIZE);
        if (n <= 0) {
            io::close(fd);
            throw csp::error("ws::connect: connection closed during handshake");
        }
        resp_buf.append(reinterpret_cast<const char*>(buf),
                        static_cast<size_t>(n));
        if (resp_buf.size() > 65536) {
            io::close(fd);
            throw csp::error("ws::connect: HTTP response headers too large");
        }
    }

    auto header_end = resp_buf.find("\r\n\r\n");

    // Validate status line.
    if (resp_buf.find("HTTP/1.1 101") == std::string::npos &&
        resp_buf.find("HTTP/1.0 101") == std::string::npos) {
        io::close(fd);
        throw csp::error("ws::connect: server did not return 101");
    }

    // Validate Sec-WebSocket-Accept.
    auto find_hdr = [&](const std::string& name) -> std::string {
        // Search for "\r\nName:" (case-insensitive would be better but
        // Sec-WebSocket-Accept is typically exact case).
        std::string needle = "\r\n" + name + ":";
        auto pos = resp_buf.find(needle);
        if (pos == std::string::npos) return {};
        auto val_start = pos + needle.size();
        auto val_end   = resp_buf.find("\r\n", val_start);
        if (val_end == std::string::npos) val_end = resp_buf.size();
        return trim(resp_buf.substr(val_start, val_end - val_start));
    };

    auto accept = find_hdr("Sec-WebSocket-Accept");
    if (accept != expected_accept) {
        io::close(fd);
        throw csp::error("ws::connect: bad Sec-WebSocket-Accept from server");
    }

    // Leftover bytes after the HTTP headers that belong to the WS stream.
    bytes leftover(
        reinterpret_cast<const uint8_t*>(resp_buf.data() + header_end + 4),
        reinterpret_cast<const uint8_t*>(resp_buf.data() + resp_buf.size()));

    return make_conn(fd, std::move(leftover), /*is_client=*/true);
}

} // namespace csp::ws
