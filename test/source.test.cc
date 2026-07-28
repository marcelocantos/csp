#include "testutil.h"


#include <atomic>
#include <cstring>
#include <stdexcept>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace csp;
using namespace csp::io;

// --- Platform pipe helpers (mirrored from io.test.cc) ---

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

inline std::pair<fd_t, fd_t> make_test_pipe() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(listener != INVALID_SOCKET);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(listener, (sockaddr*)&addr, sizeof(addr)) == 0);
    REQUIRE(::listen(listener, 1) == 0);
    int alen = sizeof(addr);
    REQUIRE(::getsockname(listener, (sockaddr*)&addr, &alen) == 0);
    SOCKET ws = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(ws != INVALID_SOCKET);
    REQUIRE(::connect(ws, (sockaddr*)&addr, sizeof(addr)) == 0);
    SOCKET rs = ::accept(listener, nullptr, nullptr);
    REQUIRE(rs != INVALID_SOCKET);
    closesocket(listener);
    return {fd_t(rs), fd_t(ws)};
}

inline void pipe_close(fd_t fd) { closesocket(fd.raw()); }
inline ssize_t pipe_write(fd_t fd, const void* buf, size_t len) {
    return ::send(fd.raw(), (const char*)buf, (int)len, 0);
}

#else

inline std::pair<fd_t, fd_t> make_test_pipe() {
    int pipefd[2];
    REQUIRE_EQ(0, pipe(pipefd));
    fd_t rfd(pipefd[0]);
    fd_t wfd(pipefd[1]);
    set_nonblock(rfd);
    set_nonblock(wfd);
    return {rfd, wfd};
}

inline void pipe_close(fd_t fd) { ::close(fd.raw()); }
inline ssize_t pipe_write(fd_t fd, const void* buf, size_t len) {
    return ::write(fd.raw(), buf, len);
}

#endif

// --- Source tests ---

// Basic: fd_source delivers bytes up to the requested count.
TEST_CASE("Source---FdSource-basic-read") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = make_test_pipe();
    const char* msg = "Hello, source!";
    size_t msglen = strlen(msg);

    pipe_write(wfd, msg, msglen);
    pipe_close(wfd);

    bytes result;
    std::atomic<bool> done{false};

    auto s = fd_source(rfd);  // source owns rfd

    csp::spawn([s = std::move(s), &result, &done]() mutable {
        // Request all bytes in one shot.
        auto reply = call_source(s, 4096);
        bytes chunk;
        if (reply >> chunk) {
            result = std::move(chunk);
        }
        done.store(true, std::memory_order_relaxed);
    });

    csp::await_completion();
    CHECK(done.load());
    std::string got(result.begin(), result.end());
    CHECK(std::string(msg) == got);
    csp::shutdown_runtime();
}

// EOF: fd_source exits cleanly when the fd reaches EOF.
// The consumer's reader<bytes> observes peer death (>> returns false).
TEST_CASE("Source---FdSource-EOF-via-death") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = make_test_pipe();

    // Write some data, then close writer to trigger EOF.
    pipe_write(wfd, "AB", 2);
    pipe_close(wfd);

    bool got_data = false;
    bool got_eof = false;
    std::atomic<bool> done{false};

    auto s = fd_source(rfd);

    csp::spawn([s = std::move(s), &got_data, &got_eof, &done]() mutable {
        // First request: should get "AB".
        {
            auto reply = call_source(s, 4096);
            bytes chunk;
            got_data = bool(reply >> chunk) && !chunk.empty();
        }
        // Second request: EOF — the imp exited; request channel is dead.
        // The << on a dead writer returns false (or throws). Use try-approach:
        // send a request and check if the reply comes back dead.
        {
            auto reply = call_source(s, 4096);
            bytes chunk;
            // This will observe either a dead request channel (the imp already
            // exited) or a dead reply writer (EOF mid-request).
            got_eof = !bool(reply >> chunk);
        }
        done.store(true, std::memory_order_relaxed);
    });

    csp::await_completion();
    CHECK(done.load());
    CHECK(got_data);
    CHECK(got_eof);
    csp::shutdown_runtime();
}

// Partial read: fd_source returns less than requested — that is normal.
TEST_CASE("Source---FdSource-partial-read") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = make_test_pipe();
    pipe_write(wfd, "XY", 2);
    pipe_close(wfd);

    std::atomic<size_t> got_bytes{0};
    std::atomic<bool> done{false};

    auto s = fd_source(rfd);

    csp::spawn([s = std::move(s), &got_bytes, &done]() mutable {
        // Request 4096, but only 2 bytes are available.
        auto reply = call_source(s, 4096);
        bytes chunk;
        if (reply >> chunk) {
            got_bytes.store(chunk.size(), std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_relaxed);
    });

    csp::await_completion();
    CHECK(done.load());
    CHECK(2 == got_bytes.load());
    csp::shutdown_runtime();
}

// Zero-byte request: fd_source throws std::invalid_argument.
TEST_CASE("Source---FdSource-zero-request-throws") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = make_test_pipe();
    pipe_close(wfd);  // wfd not needed

    bool caught_invalid_arg = false;
    std::atomic<bool> done{false};

    auto s = fd_source(rfd);

    csp::spawn([s = std::move(s), &caught_invalid_arg, &done]() mutable {
        try {
            auto reply = call_source(s, 0);  // zero-byte request
            bytes chunk;
            reply >> chunk;  // should throw std::invalid_argument
        } catch (std::invalid_argument const&) {
            caught_invalid_arg = true;
        } catch (...) {
            // unexpected exception — leave caught_invalid_arg = false
        }
        done.store(true, std::memory_order_relaxed);
    });

    csp::await_completion();
    CHECK(done.load());
    CHECK(caught_invalid_arg);
    csp::shutdown_runtime();
}

// prialt-able: two sources can be multiplexed with prialt.
TEST_CASE("Source---FdSource-prialt-two-sources") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    auto [rfd1, wfd1] = make_test_pipe();
    auto [rfd2, wfd2] = make_test_pipe();

    // Write to both pipes.
    pipe_write(wfd1, "A", 1);
    pipe_close(wfd1);
    pipe_write(wfd2, "B", 1);
    pipe_close(wfd2);

    std::string collected;
    std::atomic<bool> done{false};

    auto s1 = fd_source(rfd1);
    auto s2 = fd_source(rfd2);

    csp::spawn([s1 = std::move(s1), s2 = std::move(s2),
                &collected, &done]() mutable {
        // Fire both requests simultaneously.
        auto r1 = call_source(s1, 4096);
        auto r2 = call_source(s2, 4096);

        bytes b1, b2;

        // Drain both with prialt.  Both replies are in flight so we can
        // prialt on them together.
        int got = 0;
        while (got < 2) {
            switch (csp::prialt(r1 >> b1, r2 >> b2)) {
            case 0:
                collected += std::string(b1.begin(), b1.end());
                ++got;
                break;
            case 1:
                collected += std::string(b2.begin(), b2.end());
                ++got;
                break;
            default:
                // A reply died (EOF before any data) — count it as done.
                ++got;
                break;
            }
        }
        done.store(true, std::memory_order_relaxed);
    });

    csp::await_completion();
    CHECK(done.load());
    // Both 'A' and 'B' should have arrived, in some order.
    CHECK(2 == collected.size());
    bool has_a = collected.find('A') != std::string::npos;
    bool has_b = collected.find('B') != std::string::npos;
    CHECK(has_a);
    CHECK(has_b);
    csp::shutdown_runtime();
}

// Multi-request: consumer loops over multiple requests on one source.
TEST_CASE("Source---FdSource-multi-request-loop") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = make_test_pipe();

    // Write data in two batches.
    pipe_write(wfd, "Hello", 5);
    pipe_write(wfd, "World", 5);
    pipe_close(wfd);

    std::string accumulated;
    std::atomic<bool> done{false};

    auto s = fd_source(rfd);

    csp::spawn([s = std::move(s), &accumulated, &done]() mutable {
        for (;;) {
            auto reply = call_source(s, 5);
            bytes chunk;
            if (!(reply >> chunk)) break;  // EOF
            accumulated += std::string(chunk.begin(), chunk.end());
        }
        done.store(true, std::memory_order_relaxed);
    });

    csp::await_completion();
    CHECK(done.load());
    // The kernel may combine writes; we just need all 10 bytes.
    CHECK(10 == accumulated.size());
    csp::shutdown_runtime();
}

// writer::operator() sugar: s(n) blocks until the reply arrives.
TEST_CASE("Source---FdSource-operator()-sugar") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = make_test_pipe();
    pipe_write(wfd, "CSP", 3);
    pipe_close(wfd);

    bytes result;
    std::atomic<bool> done{false};

    auto s = fd_source(rfd);

    csp::spawn([s = std::move(s), &result, &done]() mutable {
        result = s(3);  // writer::operator() sugar for request/response
        done.store(true, std::memory_order_relaxed);
    });

    csp::await_completion();
    CHECK(done.load());
    std::string got(result.begin(), result.end());
    // Might be fewer than 3 if partial; just check non-empty.
    CHECK(!got.empty());
    csp::shutdown_runtime();
}
