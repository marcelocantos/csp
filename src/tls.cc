#ifdef CSP_TLS

#include <csp/tls.h>
#include <csp/io.h>

extern "C" {
#include <picotls.h>
#include <picotls/minicrypto.h>
}

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace csp::tls {

// --- error ---

static std::string format_error(int code) {
    if (PTLS_ERROR_GET_CLASS(code) == PTLS_ERROR_CLASS_PEER_ALERT) {
        return "tls: peer alert " + std::to_string(PTLS_ERROR_TO_ALERT(code));
    }
    if (PTLS_ERROR_GET_CLASS(code) == PTLS_ERROR_CLASS_SELF_ALERT) {
        return "tls: self alert " + std::to_string(PTLS_ERROR_TO_ALERT(code));
    }
    switch (code) {
    case PTLS_ERROR_NO_MEMORY:         return "tls: out of memory";
    case PTLS_ERROR_IN_PROGRESS:       return "tls: handshake in progress";
    case PTLS_ERROR_LIBRARY:           return "tls: library error";
    case PTLS_ERROR_INCOMPATIBLE_KEY:  return "tls: incompatible key";
    case PTLS_ERROR_PEM_LABEL_NOT_FOUND: return "tls: PEM label not found";
    default: return "tls: error " + std::to_string(code);
    }
}

error::error(int code) : csp::error(format_error(code)), code(code) {}

// --- verify_certificate bridge ---

struct verify_bridge_t {
    ptls_verify_certificate_t super;
    verify_fn fn;
};

static int verify_cb(ptls_verify_certificate_t* self, ptls_t* tls,
                     const char* server_name,
                     int (**verify_sign)(void*, uint16_t, ptls_iovec_t, ptls_iovec_t),
                     void** verify_data,
                     ptls_iovec_t* certs, size_t num_certs) {
    auto* bridge = reinterpret_cast<verify_bridge_t*>(self);

    // Build cert vector for the callback.
    std::vector<std::vector<uint8_t>> cert_chain;
    cert_chain.reserve(num_certs);
    for (size_t i = 0; i < num_certs; ++i) {
        cert_chain.emplace_back(certs[i].base, certs[i].base + certs[i].len);
    }

    // No signature verification with minicrypto — set to null.
    *verify_sign = nullptr;
    *verify_data = nullptr;

    if (!bridge->fn(server_name ? server_name : "", cert_chain)) {
        return PTLS_ALERT_TO_SELF_ERROR(PTLS_ALERT_BAD_CERTIFICATE);
    }
    return 0;
}

// --- context ---

struct context::impl {
    ptls_context_t ctx{};
    ptls_minicrypto_secp256r1sha256_sign_certificate_t sign_cert{};
    bool has_sign_cert = false;
    std::unique_ptr<verify_bridge_t> verifier;
};

context::context(role r) : impl_(std::make_unique<impl>()) {
    auto& ctx = impl_->ctx;
    ctx.random_bytes = ptls_minicrypto_random_bytes;
    ctx.get_time = &ptls_get_time;
    ctx.key_exchanges = ptls_minicrypto_key_exchanges;
    ctx.cipher_suites = ptls_minicrypto_cipher_suites;

    // Server mode doesn't request client certs by default.
    // Client mode: no built-in verification (user can set_verify).
    if (r == server) {
        ctx.require_client_authentication = 0;
    }
}

context::~context() {
    if (!impl_) return;
    auto& ctx = impl_->ctx;
    // Free sign_certificate allocated by ptls_minicrypto_load_private_key.
    free(ctx.sign_certificate);
    // Free certificate chain if loaded.
    for (size_t i = 0; i < ctx.certificates.count; ++i) {
        free(ctx.certificates.list[i].base);
    }
    free(ctx.certificates.list);
}

context::context(context&&) noexcept = default;
context& context::operator=(context&&) noexcept = default;

void context::load_cert(const char* cert_pem_path) {
    int ret = ptls_load_certificates(&impl_->ctx, cert_pem_path);
    if (ret != 0) throw error(ret);
}

void context::load_key(const char* key_pem_path) {
    int ret = ptls_minicrypto_load_private_key(&impl_->ctx, key_pem_path);
    if (ret != 0) throw error(ret);
}

void context::set_verify(verify_fn fn) {
    static const uint16_t algos[] = {
        PTLS_SIGNATURE_ECDSA_SECP256R1_SHA256,
        PTLS_SIGNATURE_RSA_PSS_RSAE_SHA256,
        UINT16_MAX,
    };
    impl_->verifier = std::make_unique<verify_bridge_t>();
    impl_->verifier->super.cb = verify_cb;
    impl_->verifier->super.algos = algos;
    impl_->verifier->fn = std::move(fn);
    impl_->ctx.verify_certificate = &impl_->verifier->super;
}

// --- socket I/O helpers ---

// Flush a PicoTLS output buffer to the socket, using csp::io for
// non-blocking writes. Returns bytes written.
static void flush_to_socket(csp::io::fd_t fd, ptls_buffer_t& buf) {
    size_t off = 0;
    while (off < buf.off) {
        csp::io::wait_writable(fd);
        ssize_t n = ::write(fd.raw(), buf.base + off, buf.off - off);
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            throw csp::error("tls: socket write failed");
        }
    }
}

// Read raw bytes from the socket into a buffer, using csp::io for
// non-blocking reads. Returns bytes read, 0 on EOF.
static ssize_t read_from_socket(csp::io::fd_t fd, uint8_t* buf, size_t len) {
    for (;;) {
        csp::io::wait_readable(fd);
        ssize_t n = ::read(fd.raw(), buf, len);
        if (n >= 0) return n;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            continue;
        throw csp::error("tls: socket read failed");
    }
}

// --- conn ---
//
// Synchronous byte-buffer TLS API.  Kept as the original direct-picotls
// implementation rather than a wrapper over tls::stream because the
// synchronous-on-write semantics (write() returns only after bytes hit
// the wire) cannot be preserved across the spawn-and-rendezvous bridge
// that a stream wrapper would introduce — a sink imp consuming
// stream::plaintext_out's ciphertext runs concurrently with the user's
// imp, so write() returning after the plaintext rendezvous does not
// imply the ciphertext has been flushed before a subsequent close(fd).
//
// Migrating conn to a stream wrapper is tracked separately; it needs
// either a request-shaped plaintext_out on tls::stream (so the user-
// side rendezvous includes sink completion) or a sync facade with a
// drain protocol bridging handshake → steady-state acks.

struct conn::impl {
    ptls_t* tls = nullptr;
    csp::io::fd_t fd;
    // Receive buffer for partially consumed TLS records (ciphertext).
    std::vector<uint8_t> recvbuf;
    size_t recvbuf_off = 0;
    // Decrypted plaintext buffer for data that was decrypted but not
    // yet returned to the caller.
    std::vector<uint8_t> plainbuf;
    size_t plainbuf_off = 0;
};

conn::conn(context& ctx, io::fd_t fd) : impl_(std::make_unique<impl>()) {
    impl_->fd = fd;

    // Suppress SIGPIPE on this fd.
#ifdef F_SETNOSIGPIPE
    fcntl(fd.raw(), F_SETNOSIGPIPE, 1);
#endif

    // Determine if server by checking if sign_certificate is set (servers load keys).
    bool is_server = (ctx.impl_->ctx.sign_certificate != nullptr);
    impl_->tls = ptls_new(&ctx.impl_->ctx, is_server ? 1 : 0);
    if (!impl_->tls) throw error(PTLS_ERROR_NO_MEMORY);
}

conn::~conn() {
    if (impl_ && impl_->tls) {
        ptls_free(impl_->tls);
    }
}

conn::conn(conn&&) noexcept = default;
conn& conn::operator=(conn&&) noexcept = default;

void conn::set_hostname(const std::string& hostname) {
    int ret = ptls_set_server_name(impl_->tls, hostname.c_str(), 0);
    if (ret != 0) throw error(ret);
}

void conn::handshake() {
    ptls_buffer_t sendbuf;
    uint8_t sendbuf_small[256];
    ptls_buffer_init(&sendbuf, sendbuf_small, sizeof(sendbuf_small));

    uint8_t readbuf[4096];
    const uint8_t* input = nullptr;
    size_t inlen = 0;

    for (;;) {
        int ret = ptls_handshake(impl_->tls, &sendbuf, input, &inlen, nullptr);

        // Always flush any output (even on error, per PicoTLS docs).
        if (sendbuf.off > 0) {
            flush_to_socket(impl_->fd, sendbuf);
            sendbuf.off = 0;
        }

        if (ret == 0) {
            // Handshake complete. If there's leftover input, save it
            // for subsequent reads.
            // Note: inlen is updated to bytes consumed; we may have
            // read more than was consumed.
            break;
        }
        if (ret != PTLS_ERROR_IN_PROGRESS) {
            ptls_buffer_dispose(&sendbuf);
            throw error(ret);
        }

        // Need more input from peer.
        ssize_t n = read_from_socket(impl_->fd, readbuf, sizeof(readbuf));
        if (n == 0) {
            ptls_buffer_dispose(&sendbuf);
            throw csp::error("tls: peer closed during handshake");
        }
        input = readbuf;
        inlen = static_cast<size_t>(n);
    }

    ptls_buffer_dispose(&sendbuf);
}

ssize_t conn::read(void* buf, size_t len) {
    // Return any previously buffered plaintext first.
    if (impl_->plainbuf_off < impl_->plainbuf.size()) {
        size_t avail = impl_->plainbuf.size() - impl_->plainbuf_off;
        size_t copy = std::min(len, avail);
        std::memcpy(buf, impl_->plainbuf.data() + impl_->plainbuf_off, copy);
        impl_->plainbuf_off += copy;
        if (impl_->plainbuf_off == impl_->plainbuf.size()) {
            impl_->plainbuf.clear();
            impl_->plainbuf_off = 0;
        }
        return static_cast<ssize_t>(copy);
    }

    ptls_buffer_t decbuf;
    uint8_t decbuf_small[256];
    ptls_buffer_init(&decbuf, decbuf_small, sizeof(decbuf_small));

    uint8_t readbuf[4096];

    auto return_plaintext = [&](size_t len) -> ssize_t {
        size_t copy = std::min(len, decbuf.off);
        std::memcpy(buf, decbuf.base, copy);
        // Buffer any excess for the next read call.
        if (copy < decbuf.off) {
            impl_->plainbuf.assign(decbuf.base + copy,
                                   decbuf.base + decbuf.off);
            impl_->plainbuf_off = 0;
        }
        ptls_buffer_dispose(&decbuf);
        return static_cast<ssize_t>(copy);
    };

    for (;;) {
        // Try to decrypt from already-buffered ciphertext.
        if (impl_->recvbuf_off < impl_->recvbuf.size()) {
            size_t avail = impl_->recvbuf.size() - impl_->recvbuf_off;
            size_t consumed = avail;
            int ret = ptls_receive(impl_->tls, &decbuf,
                                   impl_->recvbuf.data() + impl_->recvbuf_off,
                                   &consumed);
            impl_->recvbuf_off += consumed;

            // Compact buffer if fully consumed.
            if (impl_->recvbuf_off == impl_->recvbuf.size()) {
                impl_->recvbuf.clear();
                impl_->recvbuf_off = 0;
            }

            if (ret == PTLS_ALERT_TO_PEER_ERROR(PTLS_ALERT_CLOSE_NOTIFY)) {
                ptls_buffer_dispose(&decbuf);
                return 0;
            }
            if (ret != 0 && ret != PTLS_ERROR_IN_PROGRESS) {
                ptls_buffer_dispose(&decbuf);
                throw error(ret);
            }
            if (decbuf.off > 0) return return_plaintext(len);
            // Need more data — fall through to socket read.
        }

        // Read from socket.
        ssize_t n = read_from_socket(impl_->fd, readbuf, sizeof(readbuf));
        if (n == 0) {
            ptls_buffer_dispose(&decbuf);
            return 0; // EOF
        }

        // Feed to PicoTLS.
        size_t consumed = static_cast<size_t>(n);
        int ret = ptls_receive(impl_->tls, &decbuf, readbuf, &consumed);

        // Buffer any unconsumed ciphertext.
        if (consumed < static_cast<size_t>(n)) {
            impl_->recvbuf.insert(impl_->recvbuf.end(),
                                  readbuf + consumed,
                                  readbuf + n);
        }

        if (ret == PTLS_ALERT_TO_PEER_ERROR(PTLS_ALERT_CLOSE_NOTIFY)) {
            ptls_buffer_dispose(&decbuf);
            return 0;
        }
        if (ret != 0 && ret != PTLS_ERROR_IN_PROGRESS) {
            ptls_buffer_dispose(&decbuf);
            throw error(ret);
        }
        if (decbuf.off > 0) return return_plaintext(len);
        // No plaintext yet — loop to read more.
    }
}

ssize_t conn::write(const void* buf, size_t len) {
    ptls_buffer_t sendbuf;
    uint8_t sendbuf_small[256];
    ptls_buffer_init(&sendbuf, sendbuf_small, sizeof(sendbuf_small));

    int ret = ptls_send(impl_->tls, &sendbuf,
                        static_cast<const uint8_t*>(buf), len);
    if (ret != 0) {
        ptls_buffer_dispose(&sendbuf);
        throw error(ret);
    }

    flush_to_socket(impl_->fd, sendbuf);
    ptls_buffer_dispose(&sendbuf);
    return static_cast<ssize_t>(len);
}

void conn::shutdown() {
    ptls_buffer_t sendbuf;
    uint8_t sendbuf_small[64];
    ptls_buffer_init(&sendbuf, sendbuf_small, sizeof(sendbuf_small));

    // Ignore errors on close_notify — peer may have already closed.
    ptls_send_alert(impl_->tls, &sendbuf,
                    PTLS_ALERT_LEVEL_WARNING, PTLS_ALERT_CLOSE_NOTIFY);
    if (sendbuf.off > 0) {
        try {
            flush_to_socket(impl_->fd, sendbuf);
        } catch (...) {
            // Best-effort — peer may have closed.
        }
    }
    ptls_buffer_dispose(&sendbuf);
}

io::fd_t conn::fd() const {
    return impl_->fd;
}

// --- stream (🎯T17 Stages 2.2 – 2.4) ---
//
// Synchronous handshake on the calling imp, then a single steady-state
// imp that drives both directions via prialt.  Plaintext queue and
// ciphertext input buffer live in the imp; picotls owns the crypto state.
//
// Errors during handshake throw on the calling imp.  Errors during
// steady-state surface via _throw on the next plaintext read reply.
// Peer close_notify is propagated as EOF (reply-writer drop).

namespace {

constexpr size_t kHandshakeChunk = 4096;
constexpr size_t kSteadyChunk    = 8192;

using ptls_handle = std::unique_ptr<ptls_t, decltype(&ptls_free)>;

// Drain a ptls_buffer_t into a fresh bytes vector and reset the buffer.
bytes take_buffer(ptls_buffer_t& buf) {
    bytes out(buf.base, buf.base + buf.off);
    buf.off = 0;
    return out;
}

// Drive handshake synchronously.  Throws csp::error / tls::error on failure.
// Returns any leftover ciphertext bytes that arrived after the handshake
// completed — these must be fed to ptls_receive first in the steady state.
bytes drive_handshake(ptls_t* tls,
                      io::source&     ciphertext_in,
                      writer<bytes>&  ciphertext_out) {
    ptls_buffer_t sendbuf;
    uint8_t       sendbuf_small[512];
    ptls_buffer_init(&sendbuf, sendbuf_small, sizeof(sendbuf_small));

    bytes  chunk;
    size_t chunk_pos = 0;

    for (;;) {
        const uint8_t* input  = nullptr;
        size_t         inlen  = 0;
        if (chunk_pos < chunk.size()) {
            input = chunk.data() + chunk_pos;
            inlen = chunk.size() - chunk_pos;
        }

        int ret = ptls_handshake(tls, &sendbuf, input, &inlen, nullptr);
        chunk_pos += inlen;

        if (sendbuf.off > 0) {
            if (!(ciphertext_out << take_buffer(sendbuf))) {
                ptls_buffer_dispose(&sendbuf);
                throw csp::error("tls: ciphertext sink closed during handshake");
            }
        }

        if (ret == 0) {
            ptls_buffer_dispose(&sendbuf);
            bytes leftover;
            if (chunk_pos < chunk.size()) {
                leftover.assign(chunk.begin() + static_cast<ptrdiff_t>(chunk_pos),
                                chunk.end());
            }
            return leftover;
        }
        if (ret != PTLS_ERROR_IN_PROGRESS) {
            ptls_buffer_dispose(&sendbuf);
            throw error(ret);
        }

        // Need more input — pull a chunk from upstream.
        if (chunk_pos >= chunk.size()) {
            chunk.clear();
            chunk_pos = 0;
            bytes next;
            auto reply_r = io::call_source(ciphertext_in, kHandshakeChunk);
            try {
                if (!(reply_r >> next)) {
                    ptls_buffer_dispose(&sendbuf);
                    throw csp::error("tls: ciphertext source EOF during handshake");
                }
            } catch (csp::error&) {
                ptls_buffer_dispose(&sendbuf);
                throw;
            } catch (...) {
                ptls_buffer_dispose(&sendbuf);
                throw;
            }
            chunk = std::move(next);
        }
    }
}

} // namespace

stream make_stream(
    context&      ctx,
    io::source    ciphertext_in,
    writer<bytes> ciphertext_out,
    stream_params params)
{
    bool is_server = !params.client_mode;
    ptls_handle tls(ptls_new(&ctx.impl_->ctx, is_server ? 1 : 0), &ptls_free);
    if (!tls) throw error(PTLS_ERROR_NO_MEMORY);

    if (!is_server && !params.sni_hostname.empty()) {
        int ret = ptls_set_server_name(tls.get(), params.sni_hostname.c_str(), 0);
        if (ret != 0) throw error(ret);
    }

    // ALPN selection: wired in a later stage.  Stage 2.4 leaves the
    // negotiated_alpn empty regardless of params.alpn.

    bytes initial_recv = drive_handshake(tls.get(), ciphertext_in, ciphertext_out);

    chan<io::read_request> read_ch;
    chan<bytes>            write_ch;

    spawn([
        tls     = std::move(tls),
        read_r  = std::move(read_ch.r),
        write_r = std::move(write_ch.r),
        up_in   = std::move(ciphertext_in),
        up_out  = std::move(ciphertext_out),
        recvbuf  = std::move(initial_recv),
        plainbuf = bytes{}
    ]() mutable {
        internal::descr("tls::stream");

        bool peer_closed = false;

        // Decrypt from recvbuf into plainbuf until plainbuf has bytes
        // (or EOF / close_notify).  Pulls more ciphertext from upstream
        // when recvbuf empties.  Returns false on EOF / clean close.
        auto fill_plainbuf = [&]() -> bool {
            if (peer_closed) return false;
            while (plainbuf.empty()) {
                if (!recvbuf.empty()) {
                    ptls_buffer_t decbuf;
                    uint8_t       decbuf_small[2048];
                    ptls_buffer_init(&decbuf, decbuf_small, sizeof(decbuf_small));
                    size_t consumed = recvbuf.size();
                    int    ret      = ptls_receive(tls.get(), &decbuf,
                                                   recvbuf.data(), &consumed);
                    recvbuf.erase(recvbuf.begin(),
                                  recvbuf.begin() + static_cast<ptrdiff_t>(consumed));

                    if (ret == PTLS_ALERT_TO_PEER_ERROR(PTLS_ALERT_CLOSE_NOTIFY)) {
                        ptls_buffer_dispose(&decbuf);
                        peer_closed = true;
                        return false;
                    }
                    if (ret != 0 && ret != PTLS_ERROR_IN_PROGRESS) {
                        int e = ret;
                        ptls_buffer_dispose(&decbuf);
                        throw error(e);
                    }
                    if (decbuf.off > 0) {
                        plainbuf = take_buffer(decbuf);
                        ptls_buffer_dispose(&decbuf);
                        return true;
                    }
                    ptls_buffer_dispose(&decbuf);
                    // No plaintext yet — need more ciphertext.
                }

                // Pull more ciphertext.
                bytes chunk;
                auto  reply_r = io::call_source(up_in, kSteadyChunk);
                if (!(reply_r >> chunk)) {
                    // Unclean transport shutdown — treat as EOF for now.
                    // RFC 8446 §6.1 says this should be a truncation error;
                    // tightening lands in a later refinement.
                    peer_closed = true;
                    return false;
                }
                recvbuf.insert(recvbuf.end(), chunk.begin(), chunk.end());
            }
            return true;
        };

        // Encrypt plain and push to upstream.  Returns false on
        // upstream sink death.
        auto send_plain = [&](const bytes& plain) -> bool {
            ptls_buffer_t sendbuf;
            uint8_t       sendbuf_small[2048];
            ptls_buffer_init(&sendbuf, sendbuf_small, sizeof(sendbuf_small));
            int ret = ptls_send(tls.get(), &sendbuf,
                                plain.data(), plain.size());
            if (ret != 0) {
                int e = ret;
                ptls_buffer_dispose(&sendbuf);
                throw error(e);
            }
            bytes out = take_buffer(sendbuf);
            ptls_buffer_dispose(&sendbuf);
            if (out.empty()) return true;
            return static_cast<bool>(up_out << std::move(out));
        };

        // Send close_notify alert; best-effort (peer may have closed).
        // close_notify on consumer-drop is currently a no-op.  Sending
        // it through up_out reliably runs into a runtime convergence
        // issue when the ciphertext transport is a buffered chan<bytes>
        // whose reader is held but not actively reading — `up_out <<
        // alert` hangs even though the buffer has room.  Production use
        // over real sockets (kernel send buffer) wouldn't hit this path,
        // but the chan interaction needs investigation before re-enable.
        // Lifecycle is otherwise clean: the upstream peer sees the
        // ciphertext endpoints drop when the steady-state imp exits.
        auto send_close_notify = [&]() {};

        io::read_request req;
        bytes            wbuf;

        for (;;) {
            int which = prialt(read_r >> req, write_r >> wbuf);
            if (which < 0) {
                // Consumer endpoint dropped — clean TLS shutdown.
                send_close_notify();
                return;
            }

            if (which == 0) {
                // Plaintext read request.
                bool have_data = false;
                try {
                    have_data = fill_plainbuf();
                } catch (...) {
                    req.reply._throw(std::current_exception());
                    return;
                }
                if (!have_data) {
                    // EOF: drop req.reply, consumer sees death.
                    return;
                }
                size_t take = std::min(req.value, plainbuf.size());
                bytes  out(plainbuf.begin(),
                           plainbuf.begin() + static_cast<ptrdiff_t>(take));
                plainbuf.erase(plainbuf.begin(),
                               plainbuf.begin() + static_cast<ptrdiff_t>(take));
                (void)(req.reply << std::move(out));
            } else {
                // Plaintext write.
                try {
                    if (!send_plain(wbuf)) {
                        return;  // upstream sink dead
                    }
                } catch (...) {
                    // ptls_send failed — TLS state is broken, can't recover.
                    return;
                }
            }
        }
    });

    return stream{
        std::move(read_ch.w),
        std::move(write_ch.w),
        std::string{}
    };
}

} // namespace csp::tls

#endif // CSP_TLS
