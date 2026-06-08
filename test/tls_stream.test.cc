#ifdef CSP_TLS

#include "testutil.h"

#include <csp/tls.h>

#include <atomic>
#include <cstring>
#include <string>

using namespace csp;

// --- bytes_to_source: in-process adapter for tests ---
//
// Bridges a reader<bytes> into an io::source by spawning a small imp
// that services read requests out of an internal leftover buffer.
// Used to wire two `tls::stream`s together without going through
// fds or sockets — the test pipes their ciphertext through chan<bytes>.

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

// --- Tests ---

// Stage 2 stub: passthrough through a single stream.  Bytes written
// to plaintext_out emerge on the ciphertext side; bytes injected
// into the ciphertext source emerge from plaintext_in.
TEST_CASE("TlsStream---Stub-passthrough-single-stream") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    // ct_in_ch is buffered because bytes_to_source only reads on demand
    // (per request from the stream).  Sequential push-then-read inside a
    // single imp would deadlock on an unbuffered channel.
    chan<bytes> ct_in_ch(8);  // test → stream's ciphertext_in
    chan<bytes> ct_out_ch;    // stream's ciphertext_out → test

    tls::context ctx(tls::context::client);
    auto s = tls::make_stream(
        ctx,
        bytes_to_source(std::move(ct_in_ch.r)),
        std::move(ct_out_ch.w),
        {});

    std::atomic<bool> done{false};
    bytes read_back;
    bytes wrote_through;

    // Producer side: push ciphertext in, expect stream to deliver it as
    // plaintext_in; also push plaintext out, expect it on ct_out_ch.
    spawn([&,
           s = std::move(s),
           ct_in_w = std::move(ct_in_ch.w),
           ct_out_r = std::move(ct_out_ch.r)]() mutable {
        // 1. Feed bytes into ciphertext_in, read them out of plaintext_in.
        bytes msg{'H', 'i'};
        ct_in_w << msg;

        bytes got = s.plaintext_in(8);
        read_back = std::move(got);

        // 2. Push bytes into plaintext_out, read them out of ciphertext_out.
        bytes outgoing{'B', 'y', 'e'};
        s.plaintext_out << outgoing;

        bytes received;
        ct_out_r >> received;
        wrote_through = std::move(received);

        done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(done.load());
    REQUIRE(read_back.size() == 2);
    CHECK(read_back[0] == 'H');
    CHECK(read_back[1] == 'i');
    REQUIRE(wrote_through.size() == 3);
    CHECK(wrote_through[0] == 'B');
    CHECK(wrote_through[1] == 'y');
    CHECK(wrote_through[2] == 'e');
    csp::shutdown_runtime();
}

// Two stubbed streams wired back-to-back simulate a TLS-protected
// connection.  Plaintext on the client side appears as plaintext on
// the server side and vice versa, with ciphertext channels in between.
// In the stub, ciphertext is just the plaintext bytes — the wiring is
// what's exercised.
TEST_CASE("TlsStream---Stub-passthrough-loopback") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<bytes> c2s;  // client ciphertext → server
    chan<bytes> s2c;  // server ciphertext → client

    tls::context client_ctx(tls::context::client);
    tls::context server_ctx(tls::context::server);

    auto server = tls::make_stream(
        server_ctx,
        bytes_to_source(std::move(c2s.r)),
        std::move(s2c.w),
        {.client_mode = false});

    auto client = tls::make_stream(
        client_ctx,
        bytes_to_source(std::move(s2c.r)),
        std::move(c2s.w),
        {.client_mode = true});

    std::atomic<bool> client_done{false};
    std::atomic<bool> server_done{false};
    bytes server_received;
    bytes client_received;

    // Client: send "ping", read reply.
    spawn([client = std::move(client),
           &client_received, &client_done]() mutable {
        bytes ping{'p', 'i', 'n', 'g'};
        client.plaintext_out << ping;

        bytes reply = client.plaintext_in(64);
        client_received = std::move(reply);
        client_done.store(true, std::memory_order_relaxed);
    });

    // Server: read request, echo "pong".
    spawn([server = std::move(server),
           &server_received, &server_done]() mutable {
        bytes req = server.plaintext_in(64);
        server_received = std::move(req);

        bytes pong{'p', 'o', 'n', 'g'};
        server.plaintext_out << pong;
        server_done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(client_done.load());
    CHECK(server_done.load());
    REQUIRE(server_received.size() == 4);
    CHECK(std::string(server_received.begin(), server_received.end()) == "ping");
    REQUIRE(client_received.size() == 4);
    CHECK(std::string(client_received.begin(), client_received.end()) == "pong");
    csp::shutdown_runtime();
}

// When the consumer drops the plaintext_in reader after pulling some
// data, the stream imp should shut down cleanly — no hangs, no leaks.
TEST_CASE("TlsStream---Stub-consumer-drop-shuts-down") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    chan<bytes> ct_in_ch(8);  // buffered, same reason as the first test
    chan<bytes> ct_out_ch;

    tls::context ctx(tls::context::client);
    auto s = tls::make_stream(
        ctx,
        bytes_to_source(std::move(ct_in_ch.r)),
        std::move(ct_out_ch.w),
        {});

    std::atomic<bool> done{false};

    spawn([s = std::move(s),
           ct_in_w = std::move(ct_in_ch.w),
           &done]() mutable {
        ct_in_w << bytes{'x'};
        bytes got = s.plaintext_in(8);
        CHECK(got.size() == 1);
        // Drop `s` by letting the lambda exit — the stream's request
        // and write channels die; the stub imp must clean up.
        done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(done.load());
    csp::shutdown_runtime();
}

#endif // CSP_TLS
