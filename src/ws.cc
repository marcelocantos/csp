// Copyright 2025 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <csp/ws.h>
#include <csp/cancel.h>
#include <csp/internal/strutil.h>

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
#include <variant>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <ws2tcpip.h>
#endif

namespace csp::ws {

using internal::iequals;

namespace {

// RFC 6455 §1.3 handshake key GUID (one definition; 🎯T48).
constexpr const char* kWsMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Base64-encode len bytes (🎯T48: one encoder for the SHA-1 accept digest
// and the client nonce key).
std::string b64_encode(const uint8_t* data, size_t len) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len) v |= uint32_t(data[i + 2]);
        out += table[(v >> 18) & 0x3F];
        out += table[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? table[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? table[ v       & 0x3F] : '=';
    }
    return out;
}

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

    return b64_encode(digest, sizeof(digest));
}

static std::string trim(const std::string& s) {
    size_t l = s.find_first_not_of(" \t\r\n");
    if (l == std::string::npos) return {};
    size_t r = s.find_last_not_of(" \t\r\n");
    return s.substr(l, r - l + 1);
}

// ---------------------------------------------------------------------------
// Internal control message — sent from reader imp to writer imp.
//
// The reader imp never writes to the socket directly; instead it
// forwards pong and close frames to the writer imp for serialised
// delivery.  This guarantees the writer imp is the ONLY imp that
// ever calls io::write on the shared fd, eliminating the reactor
// write_writers_ insert_or_assign race when both imps try to wait
// on the same fd for writability simultaneously.
// ---------------------------------------------------------------------------

struct ctrl_pong  { bytes data; };   // send a PONG frame with this payload
struct ctrl_close { bytes data; };   // send a CLOSE frame (echo) and stop

using ctrl_msg = std::variant<ctrl_pong, ctrl_close>;

// ---------------------------------------------------------------------------
// wslay recv callback — wraps cooperative io::read.
// No send callback needed here: the reader imp never sends.
// ---------------------------------------------------------------------------

struct ws_io_read {
    io::fd_t fd;
    bytes    leftover;
    size_t   leftover_pos = 0;
};

static ssize_t ws_recv_cb(uint8_t* buf, size_t len, int /*flags*/,
                          void* user_data) {
    auto* io = static_cast<ws_io_read*>(user_data);

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

// Null send callback for the reader-side wslay context.
// The reader imp never sends frames; it only receives them.
static ssize_t ws_null_send_cb(const uint8_t* /*buf*/, size_t len,
                               int /*flags*/, void* /*user_data*/) {
    return static_cast<ssize_t>(len); // pretend we sent it (never called)
}

// genmask_callback: called when the sender needs a masking key.
static int ws_genmask_cb(uint8_t* buf, size_t len, void* /*user_data*/) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    arc4random_buf(buf, len);
#else
    for (size_t i = 0; i < len; ++i)
        buf[i] = static_cast<uint8_t>(rand());
#endif
    return 0;
}

// ---------------------------------------------------------------------------
// wslay send callbacks — used by the writer imp.
// ---------------------------------------------------------------------------

struct ws_io_write {
    io::fd_t fd;
};

static ssize_t ws_write_recv_cb(uint8_t* /*buf*/, size_t /*len*/,
                                int /*flags*/, void* /*user_data*/) {
    // The writer imp never receives frames.
    return WSLAY_ERR_WANT_READ;
}

static ssize_t ws_write_send_cb(const uint8_t* buf, size_t len, int /*flags*/,
                                void* user_data) {
    auto* io = static_cast<ws_io_write*>(user_data);
    ssize_t n = csp::io::write(io->fd, buf, len);
    if (n <= 0) return WSLAY_ERR_WANT_WRITE;
    return n;
}

// ---------------------------------------------------------------------------
// Low-level frame send helper (used by writer imp only).
// ---------------------------------------------------------------------------

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

    // The send callback (ws_write_send_cb) calls io::write which already
    // handles EAGAIN/EWOULDBLOCK by suspending the imp. Any WSLAY_ERR_WANT_WRITE
    // returned here reflects a permanent I/O failure (EPIPE, EBADF, etc.) —
    // not a transient would-block. Treat it as a fatal error and return false.
    size_t total_sent = 0;
    for (;;) {
        ssize_t r = wslay_frame_send(ctx, &iocb);
        if (r < 0) return false;  // includes WSLAY_ERR_WANT_WRITE (fatal here)

        total_sent        += static_cast<size_t>(r);
        iocb.data          = data + total_sent;
        iocb.data_length   = (len > total_sent) ? len - total_sent : 0;

        if (total_sent >= len) break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// WebSocket reader imp
//
// Reads frames from the fd. Never writes to the fd.
// Control frames are forwarded to the writer imp via ctrl_out:
//   - PING  → ctrl_pong
//   - CLOSE → ctrl_close (then reader exits)
// Data frames are forwarded to the user via data_out.
// ---------------------------------------------------------------------------

static void ws_reader(io::fd_t fd, bytes leftover,
                      writer<message>  data_out,
                      writer<ctrl_msg> ctrl_out) {
    internal::descr("ws/reader");

    wslay_frame_callbacks cbs{};
    cbs.recv_callback    = ws_recv_cb;
    cbs.send_callback    = ws_null_send_cb;   // never called
    cbs.genmask_callback = ws_genmask_cb;

    ws_io_read io_state;
    io_state.fd       = fd;
    io_state.leftover = std::move(leftover);

    wslay_frame_context_ptr ctx = nullptr;
    if (wslay_frame_context_init(&ctx, &cbs, &io_state) != 0) {
        io::close(fd);
        return;
    }

    struct ctx_guard {
        wslay_frame_context_ptr c;
        ~ctx_guard() { if (c) wslay_frame_context_free(c); }
    } guard{ctx};

    message pending;
    bool in_msg          = false;
    bytes frame_buf;
    uint64_t frame_expected = 0;

    for (;;) {
        wslay_frame_iocb iocb{};
        ssize_t r = wslay_frame_recv(ctx, &iocb);

        if (r == WSLAY_ERR_WANT_READ) break;  // EOF or error
        if (r < 0) break;                     // protocol error

        if (frame_buf.empty() && frame_expected == 0) {
            frame_expected = iocb.payload_length;
        }

        if (r > 0) {
            frame_buf.insert(frame_buf.end(),
                             iocb.data,
                             iocb.data + static_cast<size_t>(r));
        }

        bool frame_done = (frame_buf.size() >= frame_expected && r >= 0);
        if (!frame_done) continue;

        uint8_t op = iocb.opcode;

        if (wslay_is_ctrl_frame(op)) {
            if (op == WSLAY_PING) {
                // Forward to writer for serialised pong reply.
                ctrl_out << ctrl_msg{ctrl_pong{frame_buf}};
            } else if (op == WSLAY_PONG) {
                // ignore
            } else if (op == WSLAY_CONNECTION_CLOSE) {
                // Forward close echo to writer, then exit.
                ctrl_out << ctrl_msg{ctrl_close{frame_buf}};
                frame_buf.clear();
                frame_expected = 0;
                break;
            }
            frame_buf.clear();
            frame_expected = 0;
            continue;
        }

        // Data frame or continuation.
        if (op == WSLAY_TEXT_FRAME || op == WSLAY_BINARY_FRAME) {
            in_msg     = true;
            pending.op = (op == WSLAY_TEXT_FRAME) ? opcode::text : opcode::binary;
            pending.data.clear();
        }

        if (in_msg) {
            pending.data.insert(pending.data.end(),
                                frame_buf.begin(), frame_buf.end());
        }

        frame_buf.clear();
        frame_expected = 0;

        if (iocb.fin && in_msg) {
            in_msg = false;
            if (!(data_out << std::move(pending))) break;
            pending = {};
        }
    }

    // Close fd here — we own it.  The writer imp will see a write error
    // (EPIPE/EBADF) on its next io::write and exit cleanly.
    io::close(fd);
}

// ---------------------------------------------------------------------------
// WebSocket writer imp
//
// The SOLE writer to the fd. Sources:
//   1. ctrl_in: pong and close-echo frames from the reader imp.
//   2. user_in: data frames from the user (conn.send channel).
//
// Priority: ctrl_in > user_in (control frames are urgent).
//
// Lifecycle:
//   - When ctrl_in delivers ctrl_close → send close echo → exit.
//   - When user_in dies (send_w dropped by user) → send BLO Close → exit.
//   - When fd is closed by reader (io::write fails) → exit.
// ---------------------------------------------------------------------------

static void ws_writer(io::fd_t fd,
                      reader<message>  user_in,
                      reader<ctrl_msg> ctrl_in,
                      bool is_client) {
    internal::descr("ws/writer");

    wslay_frame_callbacks cbs{};
    cbs.recv_callback    = ws_write_recv_cb;
    cbs.send_callback    = ws_write_send_cb;
    cbs.genmask_callback = ws_genmask_cb;

    ws_io_write io_state;
    io_state.fd = fd;

    wslay_frame_context_ptr ctx = nullptr;
    if (wslay_frame_context_init(&ctx, &cbs, &io_state) != 0) {
        return;
    }

    struct ctx_guard {
        wslay_frame_context_ptr c;
        ~ctx_guard() { if (c) wslay_frame_context_free(c); }
    } guard{ctx};

    bool close_sent = false;

    for (;;) {
        // prialt: prefer ctrl_in (urgent control frames) over user_in.
        message  user_msg;
        ctrl_msg ctrl;

        switch (prialt(ctrl_in >> ctrl, user_in >> user_msg)) {
        case 0: {
            // ctrl_in delivered a control frame.
            if (std::holds_alternative<ctrl_pong>(ctrl)) {
                auto& p = std::get<ctrl_pong>(ctrl);
                send_frame(ctx, WSLAY_PONG, is_client,
                           p.data.data(), p.data.size());
            } else {
                // ctrl_close: echo the close frame and stop.
                auto& c = std::get<ctrl_close>(ctrl);
                if (!close_sent) {
                    send_frame(ctx, WSLAY_CONNECTION_CLOSE, is_client,
                               c.data.data(), c.data.size());
                    close_sent = true;
                }
                return;
            }
            break;
        }
        case 1: {
            // user_in delivered a data message.
            uint8_t op = (user_msg.op == opcode::text) ? WSLAY_TEXT_FRAME
                                                       : WSLAY_BINARY_FRAME;
            if (!send_frame(ctx, op, is_client,
                            user_msg.data.data(), user_msg.data.size())) {
                // Write error (fd closed by reader).
                return;
            }
            break;
        }
        case ~0:
            // ctrl_in died without delivering ctrl_close.
            // This happens when the reader exits cleanly without receiving
            // a Close frame (e.g., EOF on the socket).  Fall through to
            // handle user_in death below.
            if (!close_sent) {
                send_frame(ctx, WSLAY_CONNECTION_CLOSE, is_client, nullptr, 0);
                close_sent = true;
            }
            return;
        case ~1:
            // user_in died: BLO — send close frame to initiate close handshake.
            if (!close_sent) {
                send_frame(ctx, WSLAY_CONNECTION_CLOSE, is_client, nullptr, 0);
                close_sent = true;
            }
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Shared helper: spawn reader + writer imps from an fd.
//
// The writer imp owns no fd handle — it uses the same raw fd number as
// the reader, which owns and closes the fd.  The writer is the SOLE
// imp that writes to the fd; the reader only reads.  This eliminates
// the concurrent-write race on the kqueue reactor's write_writers_ map.
// ---------------------------------------------------------------------------

static conn make_conn(io::fd_t fd, bytes leftover, bool is_client) {
    auto [send_w, send_r] = chan<message>{};
    auto [recv_w, recv_r] = chan<message>{};
    auto [ctrl_w, ctrl_r] = chan<ctrl_msg>{};

    // Reader imp: owns fd (closes on exit).
    // Forwards data to recv_w; control frames to ctrl_w.
    csp::spawn([fd, lv = std::move(leftover),
                dw = std::move(recv_w),
                cw = std::move(ctrl_w)]() mutable {
        ws_reader(fd, std::move(lv), std::move(dw), std::move(cw));
    });

    // Writer imp: borrows the raw fd value (does NOT close it on exit).
    // Receives user messages from send_r; control frames from ctrl_r.
    csp::spawn([fd_raw = fd.raw(),
                ur = std::move(send_r),
                cr = std::move(ctrl_r),
                is_client]() mutable {
        io::fd_t view(fd_raw);  // non-owning view
        ws_writer(view, std::move(ur), std::move(cr), is_client);
    });

    return conn{std::move(recv_r), std::move(send_w)};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Server-side upgrade
// ---------------------------------------------------------------------------

conn upgrade(http::request& req) {
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
        req.hijack = {};
        throw csp::error("ws::upgrade: " + reason);
    };

    if (!iequals(trim(upgrade_hdr), "websocket"))
        bad("missing Upgrade: websocket header");

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

    std::string accept = sha1_base64(ws_key + kWsMagicGuid);

    req.respond << http::response{
        101,
        {
            {"Upgrade",              "websocket"},
            {"Connection",           "Upgrade"},
            {"Sec-WebSocket-Accept", accept},
        },
        {}
    };
    http::request::hijack_result hr;
    if (!(req.hijack >> hr)) {
        throw csp::error("ws::upgrade: failed to hijack HTTP connection");
    }

    return make_conn(hr.fd, std::move(hr.leftover), /*is_client=*/false);
}

// ---------------------------------------------------------------------------
// Raw dial: resolve host, connect, return non-blocking fd.
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
    // Expected: ws://host[:port][/path] — the authority grammar is shared
    // with http::parse_url (🎯T48).
    auto parsed = net::parse_authority(url, "ws://", 80);
    auto& host = parsed.host;
    auto  port = parsed.port;
    auto& path = parsed.path;

    // Generate a random 16-byte nonce for Sec-WebSocket-Key.
    uint8_t nonce[16];
    ws_genmask_cb(nonce, 16, nullptr);
    std::string ws_key = b64_encode(nonce, sizeof(nonce));

    std::string expected_accept = sha1_base64(ws_key + kWsMagicGuid);

    io::fd_t fd = raw_dial(host, port);

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

    // Read the server's 101 response.
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

    if (resp_buf.find("HTTP/1.1 101") == std::string::npos &&
        resp_buf.find("HTTP/1.0 101") == std::string::npos) {
        io::close(fd);
        throw csp::error("ws::connect: server did not return 101");
    }

    auto find_hdr = [&](const std::string& name) -> std::string {
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

    bytes leftover(
        reinterpret_cast<const uint8_t*>(resp_buf.data() + header_end + 4),
        reinterpret_cast<const uint8_t*>(resp_buf.data() + resp_buf.size()));

    return make_conn(fd, std::move(leftover), /*is_client=*/true);
}

} // namespace csp::ws
