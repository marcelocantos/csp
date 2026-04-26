// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// QUIC transport for CSP — ngtcp2 + PicoTLS (minicrypto backend).
//
// Architecture:
//   - One UDP socket per connection (client) or one shared UDP socket
//     for a listener (server demuxes by DCID).
//   - The reactor's existing kqueue/epoll fd-readiness path handles UDP
//     socket readability — UDP sockets behave like any other fd.
//   - Each connection runs an "io imp" that owns the ngtcp2_conn and
//     drives the protocol state machine:
//       loop:
//         prialt(udp_readable | timer_expired | outbound data poke)
//         → ngtcp2_conn_read_pkt / ngtcp2_conn_write_pkt
//   - Each QUIC stream maps to two CSP channels:
//       ngtcp2 recv callback → writer<bytes> → reader<bytes> (app)
//       app → writer<bytes> → io imp flushes via ngtcp2_conn_writev_stream
//
// Concurrency: all ngtcp2 state is owned by the io imp; channels are
// the only safe handoff points.

#ifdef CSP_TLS

#include <csp/quic.h>
#include <csp/cancel.h>
#include <csp/timer.h>
#include <csp/io.h>
#include <csp/internal/reactor.h>
#include <csp/internal/signal.h>

#include <cstdio>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

// ---- ngtcp2 ----
#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_picotls.h>

// ---- PicoTLS (minicrypto) ----
#include <picotls.h>
#include <picotls/minicrypto.h>

namespace csp::quic {

using bytes = std::vector<uint8_t>;

// ==========================================================================
// Utilities
// ==========================================================================

namespace {

ngtcp2_tstamp now_ns() {
    using namespace std::chrono;
    return static_cast<ngtcp2_tstamp>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
            .count());
}

std::string format_addr(const struct sockaddr* sa, socklen_t len) {
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    if (getnameinfo(sa, len, host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return "unknown";
    if (sa->sa_family == AF_INET6)
        return std::string("[") + host + "]:" + serv;
    return std::string(host) + ":" + serv;
}

// Create a non-blocking UDP socket bound to the given address.
int make_udp_socket(const struct sockaddr* addr, socklen_t addrlen,
                    int rcvbuf) {
    int fd = ::socket(addr->sa_family, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return -1;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    if (rcvbuf > 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    if (::bind(fd, addr, addrlen) < 0) {
        ::close(fd);
        return -1;
    }

#ifndef _WIN32
    int flags = ::fcntl(fd, F_GETFL);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    return fd;
}

// Generate a random QUIC connection ID.
ngtcp2_cid make_random_cid() {
    ngtcp2_cid cid;
    cid.datalen = NGTCP2_MIN_INITIAL_DCIDLEN;
    ptls_minicrypto_random_bytes(cid.data, cid.datalen);
    return cid;
}

// Flush all pending ngtcp2 output packets to the UDP socket.
void flush_ngtcp2_out(ngtcp2_conn* nconn, int fd,
                      const struct sockaddr* peer, socklen_t peer_len) {
    uint8_t buf[1452];
    ngtcp2_path_storage ps;
    ngtcp2_pkt_info pi{};
    ngtcp2_path_storage_zero(&ps);

    int pkts = 0;
    for (;;) {
        ngtcp2_ssize n =
            ngtcp2_conn_write_pkt(nconn, &ps.path, &pi, buf, sizeof(buf),
                                  now_ns());
        if (n < 0) { fprintf(stderr, "[flush] write_pkt err=%zd (%s)\n", n, ngtcp2_strerror((int)n)); break; }
        if (n == 0) break;
        ssize_t sent = ::sendto(fd, reinterpret_cast<const char*>(buf),
                 static_cast<int>(n), 0, peer, peer_len);
        fprintf(stderr, "[flush] sent pkt %d size=%zd sent=%zd\n", ++pkts, n, sent);
    }
    if (pkts == 0) fprintf(stderr, "[flush] no pkts to send\n");
}

// Allocate a 2-slot additional_extensions array required by the picotls
// crypto adapter before calling configure_*_session.
static ptls_raw_extension_t* alloc_additional_extensions() {
    auto* exts = new ptls_raw_extension_t[2];
    exts[0] = {0, {nullptr, 0}};
    exts[1] = {UINT16_MAX, {nullptr, 0}};
    return exts;
}

// Release a cptls context: deconfigure session (frees exts[0].data.base),
// free the extensions array, and delete the context object.
static void free_cptls(ngtcp2_crypto_picotls_ctx* cptls) {
    if (!cptls) return;
    auto* exts = cptls->handshake_properties.additional_extensions;
    ngtcp2_crypto_picotls_deconfigure_session(cptls);
    delete[] exts;
    delete cptls;
}

} // anonymous namespace

// ==========================================================================
// PicoTLS cipher configuration (minicrypto)
// ==========================================================================

namespace {

static ptls_cipher_suite_t* s_ciphers[] = {
    &ptls_minicrypto_aes128gcmsha256,
    &ptls_minicrypto_aes256gcmsha384,
    nullptr,
};

static ptls_key_exchange_algorithm_t* s_kexs[] = {
    &ptls_minicrypto_x25519,
    &ptls_minicrypto_secp256r1,
    nullptr,
};

} // anonymous namespace

// ==========================================================================
// ngtcp2_crypto_conn_ref helper
// ==========================================================================

// Helper that lets ngtcp2 callbacks resolve conn* from the ref pointer
// stored in ptls user data.
struct ConnRef {
    ngtcp2_crypto_conn_ref ref{};
    ngtcp2_conn*           conn = nullptr;

    explicit ConnRef(ngtcp2_conn* c) : conn(c) {
        ref.get_conn = [](ngtcp2_crypto_conn_ref* r) -> ngtcp2_conn* {
            return reinterpret_cast<ConnRef*>(r)->conn;
        };
    }
    // Non-copyable, non-movable (pointer identity matters).
    ConnRef(const ConnRef&) = delete;
    ConnRef& operator=(const ConnRef&) = delete;
};

// ==========================================================================
// Per-stream state
// ==========================================================================

struct StreamState {
    int64_t stream_id = -1;

    // Inbound: io imp delivers chunks here for the app to read.
    // Synchronous (unbuffered) — io imp writes after ngtcp2 callbacks
    // have returned, so it is safe to block here.
    chan<bytes> in_ch;

    // Outbound: app writes here; relay imp forwards and pokes io imp.
    // The io imp drains this between ngtcp2 calls (never inside callbacks).
    chan<bytes> out_ch;

    // Pending inbound chunks accumulated by recv_stream_data_cb.
    // Held under ConnShared::streams_mu.  Dispatched by the io imp
    // after ngtcp2_conn_read_pkt returns.
    std::vector<bytes> pending_in;
    bool               pending_fin = false;

    bool fin_sent = false;
};

// Create a writer<bytes> for the app that:
//   1) forwards each written chunk into out_ch.w (the io imp's inbound side), and
//   2) writes one byte to wake_wfd to wake the driving loop.
// The relay imp bridges the app-facing channel to the io imp's out_ch.
// make_stream_output: create a writer<bytes> that:
//   1) forwards each written chunk into out_ch (for the io imp to drain), and
//   2) writes one byte to a dup of wake_wfd to wake the driving loop.
// The relay imp owns its dup'd copy of wake_wfd and closes it on exit.
static writer<bytes> make_stream_output(writer<bytes> out_w, int wake_wfd) {
    chan<bytes> app_ch;
    auto app_r = std::move(app_ch.r);
    auto app_w = std::move(app_ch.w);

    int relay_wake_wfd = ::dup(wake_wfd); // relay imp owns this copy

    spawn([app_r   = std::move(app_r),
           out_w   = std::move(out_w),
           relay_wake_wfd]() mutable {
        bytes data;
        while (app_r >> data) {
            out_w << std::move(data);
            // Poke the driving loop (non-blocking write to pipe).
            char b = 1;
            ::write(relay_wake_wfd, &b, 1);
        }
        ::close(relay_wake_wfd);
    });

    return app_w;
}

// ==========================================================================
// Connection shared state (between io imp and app imps)
// ==========================================================================

struct ConnShared {
    // Incoming streams from peer (server side: app reads these).
    // Buffered so the listener loop can push without blocking on the app.
    chan<stream_pair> incoming_ch{16};

    // All active streams keyed by stream_id.
    // Also used to accumulate pending_in data from recv_stream_data_cb.
    std::mutex streams_mu;
    std::unordered_map<int64_t, std::shared_ptr<StreamState>> streams;

    // New stream IDs opened by the peer — set by stream_open_cb (no CSP ops
    // allowed there), dispatched to incoming_ch by the io imp after read_pkt.
    std::vector<int64_t> pending_new_streams;

    // Pending open-stream replies from io imp.
    // App imp pushes a chan<stream_pair> onto this queue, pokes,
    // and blocks on the chan; io imp creates the stream and delivers.
    std::mutex open_mu;
    std::vector<writer<stream_pair>> open_replies;

    std::shared_ptr<StreamState> get_stream(int64_t id) {
        std::lock_guard<std::mutex> lk(streams_mu);
        auto it = streams.find(id);
        return (it != streams.end()) ? it->second : nullptr;
    }

    std::shared_ptr<StreamState> make_stream(int64_t id) {
        auto ss = std::make_shared<StreamState>();
        ss->stream_id = id;
        std::lock_guard<std::mutex> lk(streams_mu);
        streams.emplace(id, ss);
        return ss;
    }

    // Remove a stream and close its channels.  Safe to call from anywhere
    // (including ngtcp2 callbacks) — the actual channel close happens under
    // the mutex-protected destructor of the returned shared_ptr.
    // The caller should let the returned ptr fall out of scope OUTSIDE any
    // ngtcp2 call to avoid closing channels from within a callback.
    std::shared_ptr<StreamState> detach_stream(int64_t id) {
        std::lock_guard<std::mutex> lk(streams_mu);
        auto it = streams.find(id);
        if (it == streams.end()) return nullptr;
        auto ss = it->second;
        streams.erase(it);
        return ss;
    }

    // Close a detached stream's channels (call after ngtcp2 returns).
    static void close_stream_channels(std::shared_ptr<StreamState>& ss) {
        if (!ss) return;
        ss->in_ch.w  = {}; // signal EOF to app reader
        ss->out_ch.r = {}; // stop relay imp
        ss.reset();
    }

    // Mark a stream for deferred closure (used inside callbacks).
    // Pending closures are held in streams_mu and resolved after read_pkt.
    std::vector<int64_t> pending_close_streams;
};

// ==========================================================================
// ngtcp2 callbacks
// ==========================================================================

namespace {

extern "C" {

static int recv_crypto_data_cb(ngtcp2_conn* conn,
                                ngtcp2_encryption_level level,
                                uint64_t /*offset*/, const uint8_t* data,
                                size_t datalen, void* /*user*/) {
    return ngtcp2_crypto_read_write_crypto_data(conn, level, data, datalen);
}

static int handshake_completed_cb(ngtcp2_conn* /*conn*/, void* /*user*/) {
    return 0;
}

static void rand_cb(uint8_t* dest, size_t destlen,
                    const ngtcp2_rand_ctx* /*ctx*/) {
    ptls_minicrypto_random_bytes(dest, destlen);
}

static int get_new_connection_id_cb(ngtcp2_conn* /*conn*/, ngtcp2_cid* cid,
                                    uint8_t* token, size_t cidlen,
                                    void* /*user*/) {
    ptls_minicrypto_random_bytes(cid->data, cidlen);
    cid->datalen = cidlen;
    ptls_minicrypto_random_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN);
    return 0;
}

static int remove_connection_id_cb(ngtcp2_conn* /*conn*/,
                                   const ngtcp2_cid* /*cid*/,
                                   void* /*user*/) {
    return 0;
}

static int update_key_cb(ngtcp2_conn* conn,
                         uint8_t* rx_secret, uint8_t* tx_secret,
                         ngtcp2_crypto_aead_ctx* rx_aead_ctx, uint8_t* rx_iv,
                         ngtcp2_crypto_aead_ctx* tx_aead_ctx, uint8_t* tx_iv,
                         const uint8_t* cur_rx_secret,
                         const uint8_t* cur_tx_secret,
                         size_t secretlen, void* /*user*/) {
    // Allocate key buffers on stack using max cipher key length.
    uint8_t rx_key[64], tx_key[64];
    return ngtcp2_crypto_update_key(
        conn, rx_secret, tx_secret,
        rx_aead_ctx, rx_key, rx_iv,
        tx_aead_ctx, tx_key, tx_iv,
        cur_rx_secret, cur_tx_secret, secretlen);
}

static int path_validation_cb(ngtcp2_conn* /*conn*/, uint32_t /*flags*/,
                               const ngtcp2_path* /*path*/,
                               const ngtcp2_path* /*old*/,
                               ngtcp2_path_validation_result /*res*/,
                               void* /*user*/) {
    return 0;
}

static int stream_open_cb(ngtcp2_conn* /*conn*/, int64_t stream_id,
                          void* user) {
    // IMPORTANT: No CSP channel ops allowed here — we are inside ngtcp2.
    // Just register the new stream; the io imp dispatches it afterward.
    auto* shared = static_cast<ConnShared*>(user);
    auto ss = std::make_shared<StreamState>();
    ss->stream_id = stream_id;
    {
        std::lock_guard<std::mutex> lk(shared->streams_mu);
        shared->streams.emplace(stream_id, ss);
        shared->pending_new_streams.push_back(stream_id);
    }
    return 0;
}

static int recv_stream_data_cb(ngtcp2_conn* /*conn*/, uint32_t flags,
                                int64_t stream_id, uint64_t /*offset*/,
                                const uint8_t* data, size_t datalen,
                                void* user, void* /*stream_user*/) {
    // IMPORTANT: No CSP channel ops allowed here — we are inside ngtcp2.
    // Accumulate data in pending_in; io imp dispatches after read_pkt.
    auto* shared = static_cast<ConnShared*>(user);
    std::lock_guard<std::mutex> lk(shared->streams_mu);
    auto it = shared->streams.find(stream_id);
    if (it == shared->streams.end()) return 0;
    auto& ss = it->second;

    if (datalen > 0)
        ss->pending_in.emplace_back(data, data + datalen);
    if (flags & NGTCP2_STREAM_DATA_FLAG_FIN)
        ss->pending_fin = true;

    return 0;
}

static int stream_close_cb(ngtcp2_conn* /*conn*/, uint32_t /*flags*/,
                            int64_t stream_id, uint64_t /*app_err*/,
                            void* user, void* /*stream_user*/) {
    // IMPORTANT: No CSP channel ops here. Defer closure to after read_pkt.
    auto* shared = static_cast<ConnShared*>(user);
    std::lock_guard<std::mutex> lk(shared->streams_mu);
    shared->pending_close_streams.push_back(stream_id);
    return 0;
}

} // extern "C"

ngtcp2_callbacks make_callbacks(bool is_server) {
    ngtcp2_callbacks cb{};
    cb.recv_crypto_data         = recv_crypto_data_cb;
    cb.handshake_completed      = handshake_completed_cb;
    cb.rand                     = rand_cb;
    cb.get_new_connection_id    = get_new_connection_id_cb;
    cb.remove_connection_id     = remove_connection_id_cb;
    cb.update_key               = update_key_cb;
    cb.path_validation          = path_validation_cb;
    cb.stream_open              = stream_open_cb;
    cb.recv_stream_data         = recv_stream_data_cb;
    cb.stream_close             = stream_close_cb;

    cb.encrypt                  = ngtcp2_crypto_encrypt_cb;
    cb.decrypt                  = ngtcp2_crypto_decrypt_cb;
    cb.hp_mask                  = ngtcp2_crypto_hp_mask_cb;
    cb.delete_crypto_aead_ctx   = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    cb.get_path_challenge_data  = ngtcp2_crypto_get_path_challenge_data_cb;
    cb.version_negotiation      = ngtcp2_crypto_version_negotiation_cb;

    if (is_server) {
        cb.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
    } else {
        cb.client_initial = ngtcp2_crypto_client_initial_cb;
    }
    cb.recv_retry = ngtcp2_crypto_recv_retry_cb;

    return cb;
}

} // anonymous namespace

// ==========================================================================
// IO imp — drives a single QUIC connection
// ==========================================================================

namespace {

// Dispatch all events accumulated during ngtcp2 callbacks to CSP channels.
// Must be called after every ngtcp2_conn_read_pkt (or similar) returns.
// wake_wfd: pipe write end — relay imps write here to wake the driving loop.
// Returns false if the incoming_streams channel has died (listener dropped).
bool dispatch_pending_events(std::shared_ptr<ConnShared>& shared, int wake_wfd) {
    // 1. New peer-opened streams → incoming_ch
    std::vector<int64_t> new_streams;
    {
        std::lock_guard<std::mutex> lk(shared->streams_mu);
        new_streams.swap(shared->pending_new_streams);
    }
    fprintf(stderr, "[dispatch] new_streams=%zu\n", new_streams.size());
    for (int64_t sid : new_streams) {
        std::shared_ptr<StreamState> ss;
        {
            std::lock_guard<std::mutex> lk(shared->streams_mu);
            auto it = shared->streams.find(sid);
            if (it == shared->streams.end()) continue;
            ss = it->second;
        }
        stream_pair sp;
        sp.input  = std::move(ss->in_ch.r);
        sp.output = make_stream_output(std::move(ss->out_ch.w), wake_wfd);
        if (!(shared->incoming_ch.w << std::move(sp)))
            return false; // listener/app dropped the connection
    }

    // 2. Inbound data → per-stream in_ch
    std::vector<std::pair<int64_t, std::shared_ptr<StreamState>>> to_deliver;
    {
        std::lock_guard<std::mutex> lk(shared->streams_mu);
        for (auto& [id, ss] : shared->streams) {
            if (!ss->pending_in.empty() || ss->pending_fin)
                to_deliver.emplace_back(id, ss);
        }
    }
    for (auto& [sid, ss] : to_deliver) {
        std::vector<bytes> chunks;
        bool fin = false;
        {
            std::lock_guard<std::mutex> lk(shared->streams_mu);
            chunks.swap(ss->pending_in);
            fin = ss->pending_fin;
            ss->pending_fin = false;
        }
        for (auto& chunk : chunks) {
            if (!(ss->in_ch.w << std::move(chunk))) {
                // App dropped the reader — close the stream.
                ss->in_ch.w = {};
                break;
            }
        }
        if (fin)
            ss->in_ch.w = {}; // EOF
    }

    // 3. Deferred stream closures
    std::vector<int64_t> closing;
    {
        std::lock_guard<std::mutex> lk(shared->streams_mu);
        closing.swap(shared->pending_close_streams);
    }
    for (int64_t sid : closing) {
        auto ss = shared->detach_stream(sid);
        ConnShared::close_stream_channels(ss);
    }

    return true;
}

// Drain pending outgoing stream data from ConnShared into ngtcp2.
// Returns false on fatal ngtcp2 error.
bool drain_stream_writes(ngtcp2_conn* nconn, std::shared_ptr<ConnShared> shared,
                         int fd, const struct sockaddr* peer, socklen_t peer_len) {
    std::vector<std::pair<int64_t, std::shared_ptr<StreamState>>> active;
    {
        std::lock_guard<std::mutex> lk(shared->streams_mu);
        for (auto& [id, ss] : shared->streams)
            active.emplace_back(id, ss);
    }

    uint8_t buf[1452];
    ngtcp2_path_storage ps;
    ngtcp2_path_storage_zero(&ps);
    ngtcp2_pkt_info pi{};

    for (auto& [sid, ss] : active) {
        bytes chunk;
        int res = prialt(ss->out_ch.r >> chunk, csp::none);
        if (res != 0) continue; // nothing ready or stream dead

        ngtcp2_vec vec{chunk.data(), chunk.size()};
        ngtcp2_ssize nw = ngtcp2_conn_writev_stream(
            nconn, &ps.path, &pi, buf, sizeof(buf),
            nullptr /* pdatalen */,
            NGTCP2_WRITE_STREAM_FLAG_NONE,
            sid, &vec, 1, now_ns());
        if (nw < 0) return false;
        if (nw > 0)
            ::sendto(fd, reinterpret_cast<const char*>(buf),
                     static_cast<int>(nw), 0, peer, peer_len);
    }
    return true;
}

// Handle pending open-stream requests from app imps.
// Returns false on fatal ngtcp2 error.
bool handle_open_requests(ngtcp2_conn* nconn,
                          std::shared_ptr<ConnShared> shared,
                          int fd, const struct sockaddr* peer,
                          socklen_t peer_len, int wake_wfd) {
    std::vector<writer<stream_pair>> requests;
    {
        std::lock_guard<std::mutex> lk(shared->open_mu);
        requests.swap(shared->open_replies);
    }
    fprintf(stderr, "[handle_open_requests] %zu requests\n", requests.size());

    for (auto& reply_w : requests) {
        int64_t stream_id = -1;
        int rv = ngtcp2_conn_open_bidi_stream(nconn, &stream_id, nullptr);
        if (rv != 0) {
            // Connection can't open more streams — drop the reply.
            continue;
        }

        auto ss = shared->make_stream(stream_id);

        stream_pair sp;
        sp.input  = std::move(ss->in_ch.r);
        sp.output = make_stream_output(std::move(ss->out_ch.w), wake_wfd);

        reply_w << std::move(sp);
    }

    flush_ngtcp2_out(nconn, fd, peer, peer_len);
    return true;
}

// Run the connection io imp.
// wake_rfd/wake_wfd: non-blocking pipe. Relay imps write to wake_wfd to
// wake this loop; the loop watches wake_rfd alongside the UDP socket fd.
// The io imp owns the pipe and closes both ends on exit.
void run_io_imp(ngtcp2_conn* nconn, ConnRef* cref,
                ngtcp2_crypto_picotls_ctx* cptls,
                ptls_t* ptls,
                std::shared_ptr<ConnShared> shared,
                int fd,
                struct sockaddr_storage peer_ss, socklen_t peer_len,
                bool owns_fd, int wake_rfd, int wake_wfd) {
    fprintf(stderr, "[io_imp] started\n");
    uint8_t rbuf[65536];
    const struct sockaddr* peer =
        reinterpret_cast<const struct sockaddr*>(&peer_ss);

    auto cleanup = [&] {
        {
            std::lock_guard<std::mutex> lk(shared->streams_mu);
            for (auto& [id, ss] : shared->streams) {
                ss->in_ch.w  = {};
                ss->out_ch.r = {};
            }
            shared->streams.clear();
        }
        ngtcp2_conn_del(nconn);
        ptls_free(ptls);
        free_cptls(cptls);
        delete cref;
        if (owns_fd) ::close(fd);
        ::close(wake_rfd);
        ::close(wake_wfd);
    };

    for (;;) {
        // Compute next timer deadline.
        ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry(nconn);
        ngtcp2_tstamp cur    = now_ns();
        int64_t delay_ns     = (expiry == UINT64_MAX) ? INT64_MAX
                               : (expiry > cur)        ? (int64_t)(expiry - cur)
                                                       : 0;

        if (delay_ns <= 0) {
            // Already expired — handle immediately without waiting.
            ngtcp2_conn_handle_expiry(nconn, now_ns());
            flush_ngtcp2_out(nconn, fd, peer, peer_len);
            continue;
        }

        // Wait for: UDP readable, timer, OR wake pipe (outbound data ready).
        detail::fd_signal udp_ready  = detail::create_fd_readable(fd);
        detail::fd_signal wake_ready = detail::create_fd_readable(wake_rfd);

        int res;
        if (delay_ns == INT64_MAX) {
            res = prialt(~udp_ready, ~wake_ready);
        } else {
            detail::timer_signal timer = detail::create_timer_signal(delay_ns);
            res = prialt(~udp_ready, ~timer, ~wake_ready);
        }

        // ~udp_ready  fires → res = ~0 = -1 (fd became readable)
        // ~timer      fires → res = ~1 = -2 (timer expired)
        // ~wake_ready fires → res = ~1 = -2 or ~2 = -3 (relay poked)
        // If ALL alternatives die simultaneously → res might be INT_MIN or similar.
        // We check which fired by the specific value.

        bool udp_fired  = (delay_ns == INT64_MAX) ? (res == ~0) : (res == ~0);
        bool timer_fired = (delay_ns != INT64_MAX) && (res == ~1);
        bool wake_fired = (delay_ns == INT64_MAX)  ? (res == ~1) : (res == ~2);
        bool all_dead = (res == INT_MIN); // no-wait returned nothing, or all died

        if (all_dead) break; // connection closed (shouldn't happen without nowait)

        if (timer_fired) {
            ngtcp2_conn_handle_expiry(nconn, now_ns());
        } else if (wake_fired) {
            // Drain the wake pipe and handle open-stream requests.
            { char buf[64]; while (::read(wake_rfd, buf, sizeof(buf)) > 0) {} }
            if (!handle_open_requests(nconn, shared, fd, peer, peer_len, wake_wfd))
                break;
        } else if (udp_fired) {
            // Drain wake pipe too (might have arrived between iterations).
            { char buf[64]; while (::read(wake_rfd, buf, sizeof(buf)) > 0) {} }
            // UDP socket readable — drain all pending datagrams.
            for (;;) {
                struct sockaddr_storage src{};
                socklen_t src_len = sizeof(src);
                ssize_t n = ::recvfrom(fd, reinterpret_cast<char*>(rbuf),
                                       sizeof(rbuf), 0,
                                       reinterpret_cast<struct sockaddr*>(&src),
                                       &src_len);
                if (n <= 0) break;

                ngtcp2_path path;
                ngtcp2_addr_init(&path.local, peer, peer_len);
                ngtcp2_addr_init(
                    &path.remote,
                    reinterpret_cast<const struct sockaddr*>(&src), src_len);

                ngtcp2_pkt_info pi{};
                int rv = ngtcp2_conn_read_pkt(nconn, &path, &pi, rbuf,
                                              static_cast<size_t>(n), now_ns());
                // Dispatch events accumulated during callbacks.
                if (!dispatch_pending_events(shared, wake_wfd)) {
                    cleanup();
                    return;
                }
                if (rv == NGTCP2_ERR_DRAINING) {
                    flush_ngtcp2_out(nconn, fd, peer, peer_len);
                    cleanup();
                    return;
                }
                if (rv != 0) {
                    cleanup();
                    return;
                }
            }
        }

        // After every event: drain pending stream writes and flush.
        if (!drain_stream_writes(nconn, shared, fd, peer, peer_len))
            break;
        flush_ngtcp2_out(nconn, fd, peer, peer_len);
    }

    cleanup();
}

} // anonymous namespace

// ==========================================================================
// connection::impl and open_stream
// ==========================================================================

struct connection::impl {
    std::shared_ptr<ConnShared> shared;
    reader<stream_pair> incoming_streams;
    std::string remote_addr;
    int wake_wfd = -1; // write end of the io imp's wake pipe

    ~impl() {
        if (wake_wfd >= 0)
            ::close(wake_wfd);
    }
};

stream_pair connection::open_stream() {
    fprintf(stderr, "[open_stream] called\n");
    if (!impl_) throw csp::error("connection is not open");

    chan<stream_pair> result_ch;
    {
        std::lock_guard<std::mutex> lk(impl_->shared->open_mu);
        impl_->shared->open_replies.push_back(std::move(result_ch.w));
    }
    // Wake the io imp via the pipe.
    if (impl_->wake_wfd >= 0) {
        char b = 1;
        ::write(impl_->wake_wfd, &b, 1);
    }

    fprintf(stderr, "[open_stream] waiting for result\n");
    stream_pair sp;
    if (!(result_ch.r >> sp))
        throw csp::error("connection closed before stream opened");
    fprintf(stderr, "[open_stream] got result\n");
    return sp;
}

// ==========================================================================
// quic::listen
// ==========================================================================

listener listen(uint16_t port, listen_options opts) {
    struct addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_PASSIVE;

    auto result = csp::io::resolve("0.0.0.0", std::to_string(port), &hints);
    if (!result)
        throw csp::error(std::string("quic::listen: resolve failed: ") +
                         result.message());

    auto* ai = result.info.get();
    int udp_fd = make_udp_socket(ai->ai_addr, ai->ai_addrlen, opts.rcvbuf);
    if (udp_fd < 0)
        throw csp::error("quic::listen: socket/bind failed");

    struct sockaddr_storage bound{};
    socklen_t bound_len = sizeof(bound);
    getsockname(udp_fd, reinterpret_cast<struct sockaddr*>(&bound), &bound_len);

    uint16_t actual_port = (bound.ss_family == AF_INET6)
        ? ntohs(reinterpret_cast<struct sockaddr_in6*>(&bound)->sin6_port)
        : ntohs(reinterpret_cast<struct sockaddr_in*>(&bound)->sin_port);
    std::string local_str = format_addr(
        reinterpret_cast<const struct sockaddr*>(&bound), bound_len);

    chan<connection> conn_out;

    // stop channel: when the listener struct is destroyed, stop_w is
    // dropped → stop_r becomes dead → ~stop_r fires → listener loop exits.
    chan<std::monostate> stop_ch;

    spawn([udp_fd, opts, bound, bound_len,
           conn_w = std::move(conn_out.w),
           stop_r = std::move(stop_ch.r)]() mutable {
        internal::descr("quic::listener");

        // PicoTLS server context.
        ptls_context_t tls_ctx{};
        tls_ctx.random_bytes  = ptls_minicrypto_random_bytes;
        tls_ctx.cipher_suites = s_ciphers;
        tls_ctx.key_exchanges = s_kexs;
        tls_ctx.get_time      = &ptls_get_time;

        // Load certificate and key if provided.
        if (!opts.cert_pem.empty()) {
            ptls_load_certificates(&tls_ctx, opts.cert_pem.c_str());
        }
        if (!opts.key_pem.empty()) {
            ptls_minicrypto_load_private_key(&tls_ctx, opts.key_pem.c_str());
        }
        ngtcp2_crypto_picotls_configure_server_context(&tls_ctx);

        // Per-DCID connection table.
        struct SConn {
            std::shared_ptr<ConnShared>  shared;
            ngtcp2_conn*                  nconn = nullptr;
            ngtcp2_crypto_picotls_ctx*    cptls = nullptr;
            ptls_t*                       ptls  = nullptr;
            ConnRef*                      cref  = nullptr;
            struct sockaddr_storage       peer{};
            socklen_t                     peer_len = 0;
            bool                          delivered = false;
        };
        std::unordered_map<std::string, std::shared_ptr<SConn>> conns;

        // Listener-wide self-pipe: relay imps write here to wake the listener
        // when outbound stream data is ready to flush.
        int listener_wake_fds[2] = {-1, -1};
        ::pipe(listener_wake_fds);
        ::fcntl(listener_wake_fds[0], F_SETFL,
                ::fcntl(listener_wake_fds[0], F_GETFL) | O_NONBLOCK);
        ::fcntl(listener_wake_fds[1], F_SETFL,
                ::fcntl(listener_wake_fds[1], F_GETFL) | O_NONBLOCK);
        int listener_wake_rfd = listener_wake_fds[0];
        int listener_wake_wfd = listener_wake_fds[1];

        uint8_t rbuf[65536];

        fprintf(stderr, "[listener] entering main loop\n");
        for (;;) {
            // Wait for UDP packet, relay-imp wake poke, OR listener drop.
            detail::fd_signal udp_sig  = detail::create_fd_readable(udp_fd);
            detail::fd_signal wake_sig = detail::create_fd_readable(listener_wake_rfd);
            fprintf(stderr, "[listener] prialt waiting (nconns=%zu)\n", conns.size());
            int res = prialt(~udp_sig, ~wake_sig, ~stop_r);
            fprintf(stderr, "[listener] prialt returned res=%d\n", res);
            // ~udp_sig  fires → res = ~0 = -1 (UDP readable)
            // ~wake_sig fires → res = ~1 = -2 (relay poked)
            // ~stop_r   fires → res = ~2 = -3 (listener dropped)
            if (res == ~1) {
                // Relay poke — drain wake pipe and flush outbound for all connections.
                { char buf[64]; while (::read(listener_wake_rfd, buf, sizeof(buf)) > 0) {} }
                for (auto& [k, sc2] : conns) {
                    if (!sc2->nconn) continue;
                    const struct sockaddr* peer2 =
                        reinterpret_cast<const struct sockaddr*>(&sc2->peer);
                    drain_stream_writes(sc2->nconn, sc2->shared,
                                        udp_fd, peer2, sc2->peer_len);
                    flush_ngtcp2_out(sc2->nconn, udp_fd, peer2, sc2->peer_len);
                }
                continue;
            }
            if (res != ~0) break; // ~2 (stop) or unexpected — exit

            // res == ~0 (-1): UDP socket readable.
            struct sockaddr_storage src{};
            socklen_t src_len = sizeof(src);
            ssize_t n = ::recvfrom(
                udp_fd, reinterpret_cast<char*>(rbuf), sizeof(rbuf), 0,
                reinterpret_cast<struct sockaddr*>(&src), &src_len);
            fprintf(stderr, "[listener] recvfrom n=%zd errno=%d\n", n, errno);
            if (n <= 0) continue;

            // Decode DCID to demultiplex.
            ngtcp2_version_cid vcid{};
            if (ngtcp2_pkt_decode_version_cid(
                    &vcid, rbuf, static_cast<size_t>(n),
                    NGTCP2_MIN_INITIAL_DCIDLEN) != 0)
                continue;

            std::string key(reinterpret_cast<const char*>(vcid.dcid),
                            vcid.dcidlen);
            auto it = conns.find(key);

            if (it == conns.end()) {
                // New connection.
                auto sc = std::make_shared<SConn>();
                sc->peer     = src;
                sc->peer_len = src_len;

                sc->ptls = ptls_new(&tls_ctx, /*is_server=*/1);
                if (!sc->ptls) continue;

                sc->cptls = new ngtcp2_crypto_picotls_ctx{};
                ngtcp2_crypto_picotls_ctx_init(sc->cptls);
                sc->cptls->ptls = sc->ptls;
                // additional_extensions must be pre-allocated (2 slots: data + sentinel).
                sc->cptls->handshake_properties.additional_extensions =
                    new ptls_raw_extension_t[2]{
                        {0, {nullptr, 0}},
                        {UINT16_MAX, {nullptr, 0}}};
                ngtcp2_crypto_picotls_configure_server_session(sc->cptls);

                // client_dcid: what the client put as its DCID (random initial value).
                // client_scid: the client's Source Connection ID.
                // For ngtcp2_conn_server_new, "dcid" = client's SCID.
                ngtcp2_cid client_dcid, client_scid, scid;
                client_dcid.datalen = vcid.dcidlen;
                memcpy(client_dcid.data, vcid.dcid, vcid.dcidlen);
                client_scid.datalen = vcid.scidlen;
                memcpy(client_scid.data, vcid.scid, vcid.scidlen);
                scid = make_random_cid();

                ngtcp2_path path{};
                ngtcp2_addr_init(
                    &path.local,
                    reinterpret_cast<const struct sockaddr*>(&bound),
                    bound_len);
                ngtcp2_addr_init(
                    &path.remote,
                    reinterpret_cast<const struct sockaddr*>(&src), src_len);

                ngtcp2_settings settings;
                ngtcp2_settings_default(&settings);
                settings.initial_ts = now_ns();

                ngtcp2_transport_params params;
                ngtcp2_transport_params_default(&params);
                params.initial_max_streams_bidi           = opts.max_streams_bidi;
                params.initial_max_data                   = 1 << 20;
                params.initial_max_stream_data_bidi_local  = 1 << 16;
                params.initial_max_stream_data_bidi_remote = 1 << 16;
                // Server must echo the client's initial DCID in transport params
                // (what the client used as the initial destination = server's address).
                params.original_dcid         = client_dcid;
                params.original_dcid_present = 1;

                sc->shared = std::make_shared<ConnShared>();
                auto cb = make_callbacks(/*is_server=*/true);

                // ngtcp2_conn_server_new: first arg "dcid" = client's Source CID.
                int rv = ngtcp2_conn_server_new(
                    &sc->nconn, &client_scid, &scid, &path,
                    NGTCP2_PROTO_VER_V1, &cb, &settings, &params,
                    nullptr, sc->shared.get());
                if (rv != 0) {
                    ptls_free(sc->ptls);
                    free_cptls(sc->cptls);
                    sc->cptls = nullptr;
                    continue;
                }

                sc->cref = new ConnRef(sc->nconn);
                *ptls_get_data_ptr(sc->ptls) = &sc->cref->ref;
                ngtcp2_conn_set_tls_native_handle(sc->nconn, sc->cptls);

                conns.emplace(key, sc);
                // Also register by server scid so subsequent client packets
                // (which use server's scid as their DCID) can find this conn.
                {
                    std::string scid_key(reinterpret_cast<const char*>(scid.data),
                                         scid.datalen);
                    conns.emplace(scid_key, sc);
                }
                it = conns.find(key);
            }

            auto& sc = *it->second;

            ngtcp2_path path{};
            ngtcp2_addr_init(
                &path.local,
                reinterpret_cast<const struct sockaddr*>(&bound), bound_len);
            ngtcp2_addr_init(
                &path.remote,
                reinterpret_cast<const struct sockaddr*>(&sc.peer), sc.peer_len);

            ngtcp2_pkt_info pi{};
            auto shared_sc = it->second->shared; // keep alive across dispatches
            fprintf(stderr, "[listener] read_pkt n=%zd\n", n);
            int rv = ngtcp2_conn_read_pkt(sc.nconn, &path, &pi, rbuf,
                                          static_cast<size_t>(n), now_ns());
            fprintf(stderr, "[listener] read_pkt rv=%d (%s)\n", rv, rv == 0 ? "OK" : ngtcp2_strerror(rv));
            // Dispatch events accumulated during callbacks (peer-opened
            // streams, inbound data, closed streams).
            // Pass listener wake pipe so relay imps wake the listener loop.
            if (!dispatch_pending_events(shared_sc, listener_wake_wfd)) {
                // incoming_ch died — listener or app dropped connection.
                break;
            }
            if (rv != 0) {
                ngtcp2_conn_del(sc.nconn);
                ptls_free(sc.ptls);
                free_cptls(sc.cptls);
                delete sc.cref;
                conns.erase(it);
                continue;
            }

            flush_ngtcp2_out(sc.nconn, udp_fd,
                             reinterpret_cast<const struct sockaddr*>(&sc.peer),
                             sc.peer_len);

            // Deliver connection once handshake completes.
            fprintf(stderr, "[listener] delivered=%d handshake_completed=%d\n",
                    sc.delivered, ngtcp2_conn_get_handshake_completed(sc.nconn));
            if (!sc.delivered &&
                ngtcp2_conn_get_handshake_completed(sc.nconn)) {
                sc.delivered = true;

                std::string raddr = format_addr(
                    reinterpret_cast<const struct sockaddr*>(&sc.peer), sc.peer_len);

                // The listener loop itself drives all server connections.
                // We deliver the connection bundle to the app.

                connection conn;
                conn.impl_ = std::make_shared<connection::impl>();
                conn.impl_->shared          = shared_sc;
                conn.impl_->remote_addr      = raddr;
                conn.incoming_streams        = std::move(shared_sc->incoming_ch.r);
                conn.remote_addr             = raddr;

                if (!(conn_w << std::move(conn))) {
                    break; // listener dropped
                }
            }

        }

        // Conns map may have multiple keys for the same SConn (client DCID +
        // server SCID); only free each once.
        std::unordered_set<SConn*> freed;
        for (auto& [k, sc] : conns) {
            if (!freed.insert(sc.get()).second) continue;
            if (sc->nconn) ngtcp2_conn_del(sc->nconn);
            if (sc->ptls)  ptls_free(sc->ptls);
            free_cptls(sc->cptls);
            delete sc->cref;
        }
        ::close(udp_fd);
        ::close(listener_wake_rfd);
        ::close(listener_wake_wfd);
    });

    listener lst;
    lst.connections = std::move(conn_out.r);
    lst.port        = actual_port;
    lst.local_addr  = std::move(local_str);
    lst.stop_w_     = std::move(stop_ch.w);
    return lst;
}

// ==========================================================================
// quic::dial
// ==========================================================================

connection dial(const std::string& host, uint16_t port, dial_options opts) {
    struct addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    auto result = csp::io::resolve(host, std::to_string(port), &hints);
    if (!result)
        throw csp::error(std::string("quic::dial: resolve failed: ") +
                         result.message());

    auto* ai = result.info.get();

    // Wildcard local address, same family as the remote.
    struct sockaddr_storage local{};
    socklen_t local_len;
    if (ai->ai_family == AF_INET6) {
        auto* a = reinterpret_cast<struct sockaddr_in6*>(&local);
        a->sin6_family = AF_INET6;
        a->sin6_port   = 0;
        local_len = sizeof(struct sockaddr_in6);
    } else {
        auto* a = reinterpret_cast<struct sockaddr_in*>(&local);
        a->sin_family = AF_INET;
        a->sin_port   = 0;
        local_len = sizeof(struct sockaddr_in);
    }

    int fd = make_udp_socket(
        reinterpret_cast<const struct sockaddr*>(&local), local_len, 0);
    if (fd < 0)
        throw csp::error("quic::dial: socket/bind failed");

    std::string raddr = format_addr(ai->ai_addr, ai->ai_addrlen);

    // PicoTLS client context — heap-allocated because ptls_t stores a pointer
    // to it and the io imp (spawned below) outlives this function frame.
    auto* tls_ctx = new ptls_context_t{};
    tls_ctx->random_bytes  = ptls_minicrypto_random_bytes;
    tls_ctx->cipher_suites = s_ciphers;
    tls_ctx->key_exchanges = s_kexs;
    tls_ctx->get_time      = &ptls_get_time;
    ngtcp2_crypto_picotls_configure_client_context(tls_ctx);

    ptls_t* ptls = ptls_new(tls_ctx, /*is_server=*/0);
    if (!ptls) {
        delete tls_ctx;
        ::close(fd);
        throw csp::error("quic::dial: ptls_new failed");
    }
    ptls_set_server_name(ptls, host.c_str(), 0);

    auto* cptls = new ngtcp2_crypto_picotls_ctx{};
    ngtcp2_crypto_picotls_ctx_init(cptls);
    cptls->ptls = ptls;
    // Pre-allocate additional_extensions (required by configure_client_session).
    cptls->handshake_properties.additional_extensions = alloc_additional_extensions();

    // Discover actual local port after bind.
    struct sockaddr_storage bound_local{};
    socklen_t bound_local_len = sizeof(bound_local);
    getsockname(fd, reinterpret_cast<struct sockaddr*>(&bound_local),
                &bound_local_len);

    ngtcp2_cid dcid = make_random_cid();
    ngtcp2_cid scid = make_random_cid();

    ngtcp2_path path{};
    ngtcp2_addr_init(
        &path.local,
        reinterpret_cast<const struct sockaddr*>(&bound_local),
        bound_local_len);
    ngtcp2_addr_init(&path.remote, ai->ai_addr, ai->ai_addrlen);

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = now_ns();

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_streams_bidi           = opts.max_streams_bidi;
    params.initial_max_data                   = 1 << 20;
    params.initial_max_stream_data_bidi_local  = 1 << 16;
    params.initial_max_stream_data_bidi_remote = 1 << 16;

    auto shared = std::make_shared<ConnShared>();
    auto cb     = make_callbacks(/*is_server=*/false);

    ngtcp2_conn* nconn = nullptr;
    int rv = ngtcp2_conn_client_new(
        &nconn, &dcid, &scid, &path,
        NGTCP2_PROTO_VER_V1, &cb, &settings, &params,
        nullptr, shared.get());
    if (rv != 0) {
        ptls_free(ptls);
        free_cptls(cptls);
        delete tls_ctx;
        ::close(fd);
        throw csp::error("quic::dial: ngtcp2_conn_client_new failed");
    }

    auto* cref = new ConnRef(nconn);
    *ptls_get_data_ptr(ptls) = &cref->ref;
    ngtcp2_conn_set_tls_native_handle(nconn, cptls);

    rv = ngtcp2_crypto_picotls_configure_client_session(cptls, nconn);
    if (rv != 0) {
        delete cref;
        ngtcp2_conn_del(nconn);
        ptls_free(ptls);
        free_cptls(cptls);
        delete tls_ctx;
        ::close(fd);
        throw csp::error("quic::dial: configure_client_session failed");
    }

    // Copy peer address into a stable storage.
    struct sockaddr_storage peer_ss{};
    memcpy(&peer_ss, ai->ai_addr, ai->ai_addrlen);
    socklen_t peer_len = ai->ai_addrlen;
    const struct sockaddr* peer = reinterpret_cast<const struct sockaddr*>(&peer_ss);

    // Flush Initial packets.
    fprintf(stderr, "[dial] flushing initial packets\n");
    flush_ngtcp2_out(nconn, fd, peer, peer_len);
    fprintf(stderr, "[dial] initial packets flushed\n");

    // Handshake loop — block the calling imp until done.
    uint8_t rbuf[65536];
    int hs_iter = 0;
    while (!ngtcp2_conn_get_handshake_completed(nconn)) {
        fprintf(stderr, "[dial] handshake iter %d, waiting for UDP\n", ++hs_iter);
        detail::fd_signal sig = detail::create_fd_readable(fd);
        if (!prialt(~sig)) {
            delete cref;
            ngtcp2_conn_del(nconn);
            ptls_free(ptls);
            free_cptls(cptls);
            delete tls_ctx;
            ::close(fd);
            throw csp::error("quic::dial: cancelled during handshake");
        }

        struct sockaddr_storage src{};
        socklen_t src_len = sizeof(src);
        ssize_t n = ::recvfrom(fd, reinterpret_cast<char*>(rbuf),
                               sizeof(rbuf), 0,
                               reinterpret_cast<struct sockaddr*>(&src),
                               &src_len);
        fprintf(stderr, "[dial] client recvfrom n=%zd errno=%d\n", n, errno);
        if (n <= 0) continue;

        ngtcp2_path ppath{};
        ngtcp2_addr_init(
            &ppath.local,
            reinterpret_cast<const struct sockaddr*>(&bound_local),
            bound_local_len);
        ngtcp2_addr_init(
            &ppath.remote,
            reinterpret_cast<const struct sockaddr*>(&src), src_len);

        ngtcp2_pkt_info pi{};
        rv = ngtcp2_conn_read_pkt(nconn, &ppath, &pi, rbuf,
                                  static_cast<size_t>(n), now_ns());
        if (rv != 0) {
            delete cref;
            ngtcp2_conn_del(nconn);
            ptls_free(ptls);
            free_cptls(cptls);
            delete tls_ctx;
            ::close(fd);
            throw csp::error(std::string("quic::dial: handshake error: ") +
                             ngtcp2_strerror(rv));
        }

        flush_ngtcp2_out(nconn, fd, peer, peer_len);
    }

    fprintf(stderr, "[dial] handshake completed!\n");
    // Create a self-pipe so relay imps and open_stream() can wake the io imp
    // when outbound data or stream requests are pending.
    // wake_rfd: io imp watches this; wake_wfd_app: app-side copy (dup'd).
    // The io imp closes both its copies; connection::impl closes the app copy.
    int wake_fds[2] = {-1, -1};
    ::pipe(wake_fds);
    ::fcntl(wake_fds[0], F_SETFL, ::fcntl(wake_fds[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(wake_fds[1], F_SETFL, ::fcntl(wake_fds[1], F_GETFL) | O_NONBLOCK);
    int wake_rfd     = wake_fds[0];
    int wake_wfd     = wake_fds[1];
    int wake_wfd_app = ::dup(wake_wfd); // app side; closed by connection::impl

    // Handshake complete — spawn the io imp.
    // tls_ctx is captured because ptls_t stores a pointer to the context;
    // we must keep it alive until ptls_free is called in the io imp.
    spawn([nconn, cref, cptls, ptls, tls_ctx, shared, fd, peer_ss, peer_len,
           wake_rfd, wake_wfd]() mutable {
        internal::descr("quic::client-io");
        run_io_imp(nconn, cref, cptls, ptls, shared, fd, peer_ss, peer_len,
                   /*owns_fd=*/true, wake_rfd, wake_wfd);
        delete tls_ctx;
    });

    connection conn;
    conn.impl_            = std::make_shared<connection::impl>();
    conn.impl_->shared    = shared;
    conn.impl_->wake_wfd  = wake_wfd_app;
    conn.incoming_streams = std::move(shared->incoming_ch.r);
    conn.remote_addr      = std::move(raddr);
    return conn;
}

} // namespace csp::quic

#endif // CSP_TLS
