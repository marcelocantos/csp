#ifdef CSP_TLS

#include "testutil.h"

#include <atomic>
#include <cstring>
#include <string>

using namespace csp;

// --- bytes_to_source: in-process adapter for tests ---
//
// Bridges a reader<bytes> into an io::source by spawning a small imp
// that services read requests from an internal leftover buffer.
// Production sources read on demand too, so the shape is fair.
//
// Used to wire two `tls::stream`s together over chan<bytes> without
// going through a real socket — the test pipes ciphertext through
// in-process channels.

static io::source bytes_to_source(reader<bytes> upstream) {
    chan<io::read_request> ch;
    spawn([req_r = std::move(ch.r),
           up = std::move(upstream),
           leftover = bytes{}]() mutable {
        internal::descr("test::bytes_to_source");
        io::read_request req;
        while (req_r >> req) {
            if (req.value == 0) {
                req.reply._throw(std::make_exception_ptr(
                    std::invalid_argument("bytes_to_source: zero request")));
                return;
            }
            if (leftover.empty()) {
                if (!(up >> leftover)) {
                    return;  // EOF — drop req.reply.
                }
            }
            size_t take = std::min(req.value, leftover.size());
            bytes out(leftover.begin(), leftover.begin() + take);
            leftover.erase(leftover.begin(),
                           leftover.begin() + static_cast<ptrdiff_t>(take));
            req.reply << std::move(out);
        }
    });
    return std::move(ch.w);
}

// Accept-all verifier for self-signed test certs.
static csp::tls::verify_fn accept_all =
    [](const char*, const auto&) { return true; };

// --- Tests ---

// Full TLS handshake + bidirectional plaintext exchange via in-process
// chan<bytes> pairs.  Both `make_stream` calls run concurrently in
// spawned imps because the handshake is interleaved; calling them
// sequentially on one imp would deadlock the first call waiting for
// the second.
TEST_CASE("TlsStream---Handshake-and-roundtrip-loopback") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    // Buffered: production sockets buffer in the kernel, so writes
    // (especially close_notify) don't typically block.  Unbuffered
    // in-process chans would deadlock on shutdown.
    chan<bytes> c2s(16);  // client ciphertext → server
    chan<bytes> s2c(16);  // server ciphertext → client

    tls::context client_ctx(tls::context::client);
    client_ctx.set_verify(accept_all);
    tls::context server_ctx(tls::context::server);
    server_ctx.load_cert("test/certs/server.crt");
    server_ctx.load_key("test/certs/server.key");

    std::atomic<bool> client_done{false};
    std::atomic<bool> server_done{false};
    bytes server_received;
    bytes client_received;

    spawn([&,
           src = bytes_to_source(std::move(c2s.r)),
           sink = std::move(s2c.w)]() mutable {
        auto server = tls::make_stream(server_ctx,
                                       std::move(src),
                                       std::move(sink),
                                       {.client_mode = false});

        bytes req = server.plaintext_in(64);
        server_received = std::move(req);

        bytes pong{'p', 'o', 'n', 'g'};
        server.plaintext_out << pong;
        server_done.store(true, std::memory_order_relaxed);
    });

    spawn([&,
           src = bytes_to_source(std::move(s2c.r)),
           sink = std::move(c2s.w)]() mutable {
        auto client = tls::make_stream(client_ctx,
                                       std::move(src),
                                       std::move(sink),
                                       {.sni_hostname = "localhost",
                                        .client_mode = true});

        bytes ping{'p', 'i', 'n', 'g'};
        client.plaintext_out << ping;

        bytes reply = client.plaintext_in(64);
        client_received = std::move(reply);
        client_done.store(true, std::memory_order_relaxed);
    });

    csp::await_completion();
    CHECK(client_done.load());
    CHECK(server_done.load());
    REQUIRE(server_received.size() == 4);
    CHECK(std::string(server_received.begin(), server_received.end()) == "ping");
    REQUIRE(client_received.size() == 4);
    CHECK(std::string(client_received.begin(), client_received.end()) == "pong");
    csp::shutdown_runtime();
}

// Multiple plaintext sends from the same side accumulate correctly.
// Tests that plainbuf/recvbuf state survives across reads on the peer.
TEST_CASE("TlsStream---Multiple-writes-aggregate") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<bytes> c2s;
    chan<bytes> s2c;

    tls::context client_ctx(tls::context::client);
    client_ctx.set_verify(accept_all);
    tls::context server_ctx(tls::context::server);
    server_ctx.load_cert("test/certs/server.crt");
    server_ctx.load_key("test/certs/server.key");

    std::atomic<bool> done{false};
    bytes accumulated;

    spawn([&,
           src = bytes_to_source(std::move(c2s.r)),
           sink = std::move(s2c.w)]() mutable {
        auto server = tls::make_stream(server_ctx,
                                       std::move(src),
                                       std::move(sink),
                                       {.client_mode = false});

        // Read three chunks; concatenate.
        for (int i = 0; i < 3; ++i) {
            bytes chunk = server.plaintext_in(64);
            accumulated.insert(accumulated.end(),
                               chunk.begin(), chunk.end());
        }
        done.store(true, std::memory_order_relaxed);
    });

    spawn([&,
           src = bytes_to_source(std::move(s2c.r)),
           sink = std::move(c2s.w)]() mutable {
        auto client = tls::make_stream(client_ctx,
                                       std::move(src),
                                       std::move(sink),
                                       {.sni_hostname = "localhost"});

        client.plaintext_out << bytes{'A', 'A'};
        client.plaintext_out << bytes{'B', 'B'};
        client.plaintext_out << bytes{'C', 'C'};
    });

    csp::await_completion();
    CHECK(done.load());
    REQUIRE(accumulated.size() == 6);
    CHECK(std::string(accumulated.begin(), accumulated.end()) == "AABBCC");
    csp::shutdown_runtime();
}

// Consumer drops the stream after one read — close_notify should fire
// and both ends should shut down cleanly without hanging.
TEST_CASE("TlsStream---Consumer-drop-sends-close-notify") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<bytes> c2s;
    chan<bytes> s2c;

    tls::context client_ctx(tls::context::client);
    client_ctx.set_verify(accept_all);
    tls::context server_ctx(tls::context::server);
    server_ctx.load_cert("test/certs/server.crt");
    server_ctx.load_key("test/certs/server.key");

    std::atomic<bool> client_done{false};
    std::atomic<bool> server_saw_eof{false};

    spawn([&,
           src = bytes_to_source(std::move(c2s.r)),
           sink = std::move(s2c.w)]() mutable {
        auto server = tls::make_stream(server_ctx,
                                       std::move(src),
                                       std::move(sink),
                                       {.client_mode = false});

        // Read once, then read again expecting EOF (peer close_notify).
        bytes first = server.plaintext_in(64);
        CHECK(first.size() == 2);

        auto reply = io::call_source(server.plaintext_in, 64);
        bytes second;
        if (!(reply >> second)) {
            server_saw_eof.store(true, std::memory_order_relaxed);
        }
    });

    spawn([&,
           src = bytes_to_source(std::move(s2c.r)),
           sink = std::move(c2s.w)]() mutable {
        auto client = tls::make_stream(client_ctx,
                                       std::move(src),
                                       std::move(sink),
                                       {.sni_hostname = "localhost"});

        client.plaintext_out << bytes{'h', 'i'};
        // Drop client by letting the lambda exit — stream imp sends
        // close_notify, then exits.
        client_done.store(true, std::memory_order_relaxed);
    });

    csp::await_completion();
    CHECK(client_done.load());
    CHECK(server_saw_eof.load());
    csp::shutdown_runtime();
}

// Regression for 🎯T17.3: mutual close_notify must not deadlock.
//
// Both peers finish their work and then each tries to send a
// close_notify into a chan<bytes> whose peer has already stopped pulling
// ciphertext.  With a *blocking* close_notify send both stream imps
// would wedge forever (each blocked on `up_out << alert`).  The
// non-blocking best-effort send breaks the cycle: an alert that can't be
// delivered immediately is simply dropped, and the peer observes EOF via
// the ciphertext endpoints dropping when the imp exits.
//
// This is the shape that the original no-op stub punted on; it must run
// to completion without the test harness having to time it out.
TEST_CASE("TlsStream---Mutual-close-notify-no-deadlock") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<bytes> c2s;   // unbuffered: no kernel-buffer cushion
    chan<bytes> s2c;

    tls::context client_ctx(tls::context::client);
    client_ctx.set_verify(accept_all);
    tls::context server_ctx(tls::context::server);
    server_ctx.load_cert("test/certs/server.crt");
    server_ctx.load_key("test/certs/server.key");

    std::atomic<bool> client_done{false};
    std::atomic<bool> server_done{false};

    // Server: read one chunk, then drop — triggering its own
    // close_notify while the client is also dropping.
    spawn([&,
           src = bytes_to_source(std::move(c2s.r)),
           sink = std::move(s2c.w)]() mutable {
        auto server = tls::make_stream(server_ctx,
                                       std::move(src),
                                       std::move(sink),
                                       {.client_mode = false});
        bytes chunk = server.plaintext_in(64);
        CHECK(chunk.size() == 2);
        server_done.store(true, std::memory_order_relaxed);
        // Drop server on scope exit → send_close_notify into s2c, whose
        // reader (the client's bytes_to_source) has stopped pulling.
    });

    // Client: send one chunk, then drop.
    spawn([&,
           src = bytes_to_source(std::move(s2c.r)),
           sink = std::move(c2s.w)]() mutable {
        auto client = tls::make_stream(client_ctx,
                                       std::move(src),
                                       std::move(sink),
                                       {.sni_hostname = "localhost"});
        client.plaintext_out << bytes{'h', 'i'};
        client_done.store(true, std::memory_order_relaxed);
        // Drop client on scope exit → send_close_notify into c2s, whose
        // reader (the server's bytes_to_source) has stopped pulling.
    });

    // If close_notify blocks, await_completion() never returns and the suite
    // hangs; reaching the asserts below is itself the regression check.
    csp::await_completion();
    CHECK(client_done.load());
    CHECK(server_done.load());
    csp::shutdown_runtime();
}

// Buffered transport: the consumer drops while the peer is idle, then
// the peer pulls afterward and must observe a clean shutdown.  Exercises
// the path the original stub *feared* — close_notify over a buffered
// chan<bytes> whose reader is alive but not actively pulling at send
// time.  The non-blocking send lands the alert in the buffer's slot
// (the buffer filter is parked at its read rendezvous), so the peer
// decrypts it as a graceful close on its next pull.
TEST_CASE("TlsStream---Close-notify-over-buffered-transport") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<bytes> c2s(16);   // buffered: alert lands in a free slot
    chan<bytes> s2c(16);

    tls::context client_ctx(tls::context::client);
    client_ctx.set_verify(accept_all);
    tls::context server_ctx(tls::context::server);
    server_ctx.load_cert("test/certs/server.crt");
    server_ctx.load_key("test/certs/server.key");

    std::atomic<bool> client_done{false};
    std::atomic<bool> server_saw_eof{false};

    spawn([&,
           src = bytes_to_source(std::move(c2s.r)),
           sink = std::move(s2c.w)]() mutable {
        auto server = tls::make_stream(server_ctx,
                                       std::move(src),
                                       std::move(sink),
                                       {.client_mode = false});
        bytes first = server.plaintext_in(64);
        CHECK(first.size() == 2);
        auto reply = io::call_source(server.plaintext_in, 64);
        bytes second;
        if (!(reply >> second)) {
            server_saw_eof.store(true, std::memory_order_relaxed);
        }
    });

    spawn([&,
           src = bytes_to_source(std::move(s2c.r)),
           sink = std::move(c2s.w)]() mutable {
        auto client = tls::make_stream(client_ctx,
                                       std::move(src),
                                       std::move(sink),
                                       {.sni_hostname = "localhost"});
        client.plaintext_out << bytes{'h', 'i'};
        client_done.store(true, std::memory_order_relaxed);
    });

    csp::await_completion();
    CHECK(client_done.load());
    CHECK(server_saw_eof.load());
    csp::shutdown_runtime();
}

#endif // CSP_TLS
