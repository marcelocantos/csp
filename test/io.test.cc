#include "testutil.h"

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#endif

using namespace csp;
using namespace csp::part;

// --- Platform helpers for tests ---

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

// Create a connected loopback TCP socket pair (Windows substitute for pipe()).
inline std::pair<csp::io::socket_t, csp::io::socket_t> test_pipe() {
    // Ensure Winsock is initialized.
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(listener != INVALID_SOCKET);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(listener, 1) == 0);

    int addrlen = sizeof(addr);
    REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrlen) == 0);

    SOCKET writer_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(writer_sock != INVALID_SOCKET);
    REQUIRE(::connect(writer_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

    SOCKET reader_sock = ::accept(listener, nullptr, nullptr);
    REQUIRE(reader_sock != INVALID_SOCKET);
    closesocket(listener);

    return {reader_sock, writer_sock};
}

inline void test_close(csp::io::socket_t fd) { closesocket(fd); }

inline ssize_t test_raw_write(csp::io::socket_t fd, const void* buf, size_t len) {
    return ::send(fd, static_cast<const char*>(buf), static_cast<int>(len), 0);
}

inline ssize_t test_raw_read(csp::io::socket_t fd, void* buf, size_t len) {
    return ::recv(fd, static_cast<char*>(buf), static_cast<int>(len), 0);
}

#else // !_WIN32

inline std::pair<csp::io::socket_t, csp::io::socket_t> test_pipe() {
    int pipefd[2];
    REQUIRE_EQ(0, pipe(pipefd));
    return {pipefd[0], pipefd[1]};
}

inline void test_close(csp::io::socket_t fd) { ::close(fd); }

inline ssize_t test_raw_write(csp::io::socket_t fd, const void* buf, size_t len) {
    return ::write(fd, buf, len);
}

inline ssize_t test_raw_read(csp::io::socket_t fd, void* buf, size_t len) {
    return ::read(fd, buf, len);
}

#endif // _WIN32

// --- Layer 1: wait_readable / wait_writable ---

TEST_CASE("IO - WaitReadable") {
    csp::init_runtime(2);

    auto [rfd, wfd] = test_pipe();
    csp::io::set_nonblock(rfd);
    csp::io::set_nonblock(wfd);

    std::atomic<bool> got_data{false};

    csp::spawn([&got_data, rfd] {
        csp::io::wait_readable(rfd);
        char buf[16];
        ssize_t n = test_raw_read(rfd, buf, sizeof(buf));
        CHECK_GT(n, 0);
        CHECK_EQ('X', buf[0]);
        got_data.store(true, std::memory_order_relaxed);
        test_close(rfd);
    });

    csp::spawn([wfd] {
        csp::sleep(std::chrono::milliseconds(10));
        test_raw_write(wfd, "X", 1);
        test_close(wfd);
    });

    csp::schedule();
    CHECK(got_data.load());
    csp::shutdown_runtime();
}

TEST_CASE("IO - WaitWritable") {
    csp::init_runtime(2);

    auto [rfd, wfd] = test_pipe();
    csp::io::set_nonblock(rfd);
    csp::io::set_nonblock(wfd);

    std::atomic<bool> write_completed{false};

    // Write a byte — should succeed immediately.
    csp::spawn([&write_completed, wfd] {
        char c = 'Y';
        csp::io::wait_writable(wfd);
        ssize_t n = test_raw_write(wfd, &c, 1);
        CHECK_EQ(1, n);
        write_completed.store(true, std::memory_order_relaxed);
        test_close(wfd);
    });

    csp::spawn([rfd] {
        csp::sleep(std::chrono::milliseconds(10));
        char buf[16];
        test_raw_read(rfd, buf, sizeof(buf));
        test_close(rfd);
    });

    csp::schedule();
    CHECK(write_completed.load());
    csp::shutdown_runtime();
}

// --- Layer 2: io::read / io::write ---

TEST_CASE("IO - ReadWrite roundtrip") {
    csp::init_runtime(2);

    auto [rfd, wfd] = test_pipe();
    csp::io::set_nonblock(rfd);
    csp::io::set_nonblock(wfd);

    const char* msg = "Hello, CSP I/O!";
    size_t msglen = strlen(msg);
    std::vector<char> result(msglen);
    std::atomic<bool> done{false};

    csp::spawn([wfd, msg, msglen] {
        ssize_t n = csp::io::write(wfd, msg, msglen);
        CHECK_EQ(static_cast<ssize_t>(msglen), n);
        test_close(wfd);
    });

    csp::spawn([&result, &done, rfd, msglen] {
        size_t total = 0;
        while (total < msglen) {
            ssize_t n = csp::io::read(rfd, result.data() + total, msglen - total);
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }
        CHECK_EQ(msglen, total);
        done.store(true, std::memory_order_relaxed);
        test_close(rfd);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK_EQ(std::string(msg), std::string(result.data(), result.size()));
    csp::shutdown_runtime();
}

// --- Layer 3: byte_reader ---

TEST_CASE("IO - ByteReader") {
    csp::init_runtime(2);

    auto [rfd, wfd] = test_pipe();

    // byte_reader owns rfd and closes it.
    auto r = part::io::byte_reader(rfd, 16).spawn();

    bytes all;
    std::atomic<bool> done{false};

    csp::spawn([wfd] {
        const char* msg = "Hello, CSP!";
        test_raw_write(wfd, msg, strlen(msg));
        test_close(wfd);
    });

    csp::spawn([&all, &done, r = std::move(r)] {
        for (bytes chunk; r >> chunk;) {
            all.insert(all.end(), chunk.begin(), chunk.end());
        }
        done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(done.load());
    std::string result(all.begin(), all.end());
    CHECK_EQ("Hello, CSP!", result);
    csp::shutdown_runtime();
}

// --- Layer 3: byte_writer ---

TEST_CASE("IO - ByteWriter") {
    csp::init_runtime(2);

    auto [rfd, wfd] = test_pipe();

    // byte_writer owns wfd and closes it.
    auto w = part::io::byte_writer(wfd).spawn();

    std::atomic<bool> done{false};
    std::vector<char> result;

    csp::spawn([w = std::move(w)] {
        std::string msg = "CSP writes!";
        bytes chunk(msg.begin(), msg.end());
        w << std::move(chunk);
    });

    csp::spawn([&result, &done, rfd] {
        char buf[64];
        ssize_t total = 0;
        for (;;) {
            ssize_t n = test_raw_read(rfd, buf + total, sizeof(buf) - total);
            if (n <= 0) break;
            total += n;
        }
        result.assign(buf, buf + total);
        done.store(true, std::memory_order_relaxed);
        test_close(rfd);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK_EQ("CSP writes!", std::string(result.data(), result.size()));
    csp::shutdown_runtime();
}

// --- byte_reader (channel-backed) ---

TEST_CASE("IO - byte_reader exact chunk") {
    auto [w, r] = chan<bytes>{};
    byte_reader br(std::move(r));

    csp::spawn([w = std::move(w)] {
        w << bytes{'A', 'B', 'C', 'D'};
    });

    bytes buf(4);
    CHECK_EQ(4, br.read(buf));
    CHECK_EQ(bytes({'A', 'B', 'C', 'D'}), buf);
}

TEST_CASE("IO - byte_reader spans chunks") {
    auto [w, r] = chan<bytes>{};
    byte_reader br(std::move(r));

    csp::spawn([w = std::move(w)] {
        w << bytes{'A', 'B'};
        w << bytes{'C', 'D'};
        w << bytes{'E', 'F'};
    });

    bytes buf(5);
    CHECK_EQ(5, br.read(buf));
    CHECK_EQ(bytes({'A', 'B', 'C', 'D', 'E'}), buf);

    // Leftover 'F' from previous read.
    bytes buf2(1);
    CHECK_EQ(1, br.read(buf2));
    CHECK_EQ(bytes({'F'}), buf2);
}

TEST_CASE("IO - byte_reader partial on close") {
    auto [w, r] = chan<bytes>{};
    byte_reader br(std::move(r));

    csp::spawn([w = std::move(w)] {
        w << bytes{'X', 'Y'};
    });

    bytes buf(10);
    CHECK_EQ(2, br.read(buf));
    CHECK_EQ('X', buf[0]);
    CHECK_EQ('Y', buf[1]);
}

TEST_CASE("IO - byte_reader empty buffer") {
    auto [w, r] = chan<bytes>{};
    byte_reader br(std::move(r));

    bytes buf;
    CHECK_EQ(0, br.read(buf));
}

TEST_CASE("IO - byte_reader multiple reads with leftover") {
    auto [w, r] = chan<bytes>{};
    byte_reader br(std::move(r));

    csp::spawn([w = std::move(w)] {
        // Send 10 bytes in one chunk.
        w << bytes{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    });

    bytes buf(3);

    CHECK_EQ(3, br.read(buf));
    CHECK_EQ(bytes({'0', '1', '2'}), buf);

    CHECK_EQ(3, br.read(buf));
    CHECK_EQ(bytes({'3', '4', '5'}), buf);

    CHECK_EQ(3, br.read(buf));
    CHECK_EQ(bytes({'6', '7', '8'}), buf);

    // Only 1 byte left.
    CHECK_EQ(1, br.read(buf));
    CHECK_EQ('9', buf[0]);
}

TEST_CASE("IO - byte_reader closed reader returns zero") {
    auto [w, r] = chan<bytes>{};
    byte_reader br(std::move(r));

    // Close the writer immediately.
    { auto _ = std::move(w); }

    bytes buf(4);
    CHECK_EQ(0, br.read(buf));
}

// --- Layer 3: split_lines — pure channel test, no I/O ---

TEST_CASE("IO - Lines framing") {
    auto [w, r] = chan<bytes>{};
    auto lr = part::io::split_lines.spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        std::string data = "hello\nworld\nfoo\n";
        bytes v(data.begin(), data.end());
        w << std::move(v);
    });

    CHECK_EQ("hello", lr.read());
    CHECK_EQ("world", lr.read());
    CHECK_EQ("foo", lr.read());
}

TEST_CASE("IO - Lines partial flush") {
    auto [w, r] = chan<bytes>{};
    auto lr = part::io::split_lines.spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        std::string data = "hello\nworld";
        bytes v(data.begin(), data.end());
        w << std::move(v);
    });

    CHECK_EQ("hello", lr.read());
    CHECK_EQ("world", lr.read());  // partial line flushed on input close
    std::string _;
    CHECK_FALSE(bool(lr >> _));
}

TEST_CASE("IO - Lines multi-chunk") {
    auto [w, r] = chan<bytes>{};
    auto lr = part::io::split_lines.spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        // Split "hello\nworld\n" across two chunks.
        std::string c1 = "hel";
        std::string c2 = "lo\nworld\n";
        w << bytes(c1.begin(), c1.end());
        w << bytes(c2.begin(), c2.end());
    });

    CHECK_EQ("hello", lr.read());
    CHECK_EQ("world", lr.read());
    std::string _;
    CHECK_FALSE(bool(lr >> _));
}

// --- Layer 3: fixed_frames() — pure channel test ---

TEST_CASE("IO - Fixed framing") {
    auto [w, r] = chan<bytes>{};
    auto fr = part::io::fixed_frames(4).spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        // 10 bytes → 2 full frames of 4, partial 2 discarded.
        std::string data = "AABBCCDDEE";
        w << bytes(data.begin(), data.end());
    });

    auto f1 = fr.read();
    CHECK_EQ(4, f1.size());
    CHECK_EQ('A', f1[0]); CHECK_EQ('A', f1[1]);
    CHECK_EQ('B', f1[2]); CHECK_EQ('B', f1[3]);

    auto f2 = fr.read();
    CHECK_EQ(4, f2.size());
    CHECK_EQ('C', f2[0]); CHECK_EQ('C', f2[1]);
    CHECK_EQ('D', f2[2]); CHECK_EQ('D', f2[3]);

    // Partial "EE" is discarded.
    bytes _;
    CHECK_FALSE(bool(fr >> _));
}

TEST_CASE("IO - Fixed multi-chunk") {
    auto [w, r] = chan<bytes>{};
    auto fr = part::io::fixed_frames(4).spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        // Frame boundary spans chunks.
        w << bytes{'A', 'B'};
        w << bytes{'C', 'D', 'E', 'F'};
    });

    auto f1 = fr.read();
    CHECK_EQ(4, f1.size());
    CHECK_EQ('A', f1[0]); CHECK_EQ('B', f1[1]);
    CHECK_EQ('C', f1[2]); CHECK_EQ('D', f1[3]);

    // Partial "EF" is discarded.
    bytes _;
    CHECK_FALSE(bool(fr >> _));
}

// --- Composed pipeline: split_lines(byte_reader(fd)) ---

TEST_CASE("IO - Composed lines from pipe") {
    csp::init_runtime(2);

    auto [rfd, wfd] = test_pipe();

    auto lr = part::io::split_lines.spawn(part::io::byte_reader(rfd).spawn());

    std::vector<std::string> result;
    std::atomic<bool> done{false};

    csp::spawn([wfd] {
        const char* data = "alpha\nbeta\ngamma\n";
        test_raw_write(wfd, data, strlen(data));
        test_close(wfd);
    });

    csp::spawn([&result, &done, lr = std::move(lr)] {
        for (std::string line; lr >> line;) {
            result.push_back(std::move(line));
        }
        done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(done.load());
    REQUIRE_EQ(3, result.size());
    CHECK_EQ("alpha", result[0]);
    CHECK_EQ("beta", result[1]);
    CHECK_EQ("gamma", result[2]);
    csp::shutdown_runtime();
}

// --- Multiple concurrent I/O waiters ---

TEST_CASE("IO - Multiple concurrent waiters") {
    csp::init_runtime(4);

    constexpr int N = 8;
    csp::io::socket_t rfds[N], wfds[N];
    for (int i = 0; i < N; ++i) {
        auto [r, w] = test_pipe();
        rfds[i] = r;
        wfds[i] = w;
        csp::io::set_nonblock(rfds[i]);
        csp::io::set_nonblock(wfds[i]);
    }

    std::atomic<int> count{0};

    for (int i = 0; i < N; ++i) {
        csp::spawn([&count, rfd = rfds[i]] {
            csp::io::wait_readable(rfd);
            char buf[4];
            test_raw_read(rfd, buf, sizeof(buf));
            count.fetch_add(1, std::memory_order_relaxed);
            test_close(rfd);
        });
    }

    // Delay, then write to all pipes.
    csp::spawn([&wfds] {
        csp::sleep(std::chrono::milliseconds(20));
        for (int i = 0; i < N; ++i) {
            test_raw_write(wfds[i], "!", 1);
            test_close(wfds[i]);
        }
    });

    csp::schedule();
    CHECK_EQ(N, count.load());
    csp::shutdown_runtime();
}

// --- csp::blocking ---

TEST_CASE("IO - Blocking offload") {
    csp::init_runtime(2);

    std::atomic<bool> ran{false};
    std::atomic<std::thread::id> pool_tid{};

    csp::spawn([&] {
        auto main_tid = std::this_thread::get_id();
        int result = csp::blocking([&] {
            pool_tid.store(std::this_thread::get_id(), std::memory_order_relaxed);
            ran.store(true, std::memory_order_relaxed);
            return 42;
        });
        CHECK_EQ(42, result);
        // The blocking lambda ran on a different thread.
        CHECK_NE(main_tid, pool_tid.load());
    });

    csp::schedule();
    CHECK(ran.load());
    csp::shutdown_runtime();
}

// --- DNS resolution ---

TEST_CASE("IO - Resolve unresolvable hostname") {
    csp::init_runtime(2);

    std::atomic<bool> done{false};

    csp::spawn([&] {
        auto r = csp::io::resolve("this.host.definitely.does.not.exist.invalid");
        CHECK_FALSE(bool(r));
        CHECK(r.error != 0);
        std::string msg = r.message();
        CHECK_FALSE(msg.empty());
        done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(done.load());
    csp::shutdown_runtime();
}

TEST_CASE("IO - Resolve localhost") {
    csp::init_runtime(2);

    std::atomic<bool> resolved{false};

    csp::spawn([&] {
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        auto r = csp::io::resolve("localhost", "80", &hints);
        CHECK(r);
        CHECK(r.info != nullptr);
        resolved.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(resolved.load());
    csp::shutdown_runtime();
}

// --- csp::blocking edge cases ---

TEST_CASE("IO - Blocking void return") {
    csp::init_runtime(2);

    std::atomic<bool> ran{false};

    csp::spawn([&] {
        csp::blocking([&] {
            ran.store(true, std::memory_order_relaxed);
        });
    });

    csp::schedule();
    CHECK(ran.load());
    csp::shutdown_runtime();
}

TEST_CASE("IO - Blocking with non-trivial return type") {
    csp::init_runtime(2);

    std::atomic<bool> done{false};

    csp::spawn([&] {
        std::string result = csp::blocking([] {
            return std::string("hello from pool");
        });
        CHECK_EQ("hello from pool", result);
        done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(done.load());
    csp::shutdown_runtime();
}

TEST_CASE("IO - Multiple concurrent blocking calls") {
    csp::init_runtime(4);

    constexpr int N = 5;
    std::atomic<int> completed{0};

    for (int i = 0; i < N; ++i) {
        csp::spawn([&completed, i] {
            int result = csp::blocking([i] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return i * 10;
            });
            CHECK_EQ(i * 10, result);
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();
    CHECK_EQ(N, completed.load());
    csp::shutdown_runtime();
}

// --- Signal channels (Unix only) ---

#ifndef _WIN32

TEST_CASE("IO - Signal delivery") {
    csp::init_runtime(2);

    auto sig = csp::signal::notify({SIGUSR1});
    std::atomic<bool> got_signal{false};

    csp::spawn([&got_signal, sig = std::move(sig)] {
        int s;
        sig >> s;
        CHECK_EQ(SIGUSR1, s);
        got_signal.store(true, std::memory_order_relaxed);
    });

    csp::spawn([] {
        csp::sleep(std::chrono::milliseconds(10));
        ::raise(SIGUSR1);
    });

    csp::schedule();
    CHECK(got_signal.load());
    csp::shutdown_runtime();
}

TEST_CASE("IO - Signal multiple signals") {
    csp::init_runtime(2);

    auto sig = csp::signal::notify({SIGUSR1, SIGUSR2});
    std::vector<int> received;
    std::atomic<bool> done{false};

    csp::spawn([&received, &done, sig = std::move(sig)] {
        int s;
        // Read two signals.
        sig >> s;
        received.push_back(s);
        sig >> s;
        received.push_back(s);
        done.store(true, std::memory_order_relaxed);
    });

    csp::spawn([] {
        csp::sleep(std::chrono::milliseconds(10));
        ::raise(SIGUSR1);
        csp::sleep(std::chrono::milliseconds(10));
        ::raise(SIGUSR2);
    });

    csp::schedule();
    CHECK(done.load());
    REQUIRE_EQ(2, received.size());
    CHECK_EQ(SIGUSR1, received[0]);
    CHECK_EQ(SIGUSR2, received[1]);
    csp::shutdown_runtime();
}

TEST_CASE("IO - Signal reader drop cleanup") {
    csp::init_runtime(2);

    {
        auto sig = csp::signal::notify({SIGUSR1});
        // Drop immediately — sentinel closes the pipe, MTs exit.
    }

    // Wait for producer + sentinel MTs to finish.
    csp::schedule();

    // Signal after cleanup — handler skips (mask cleared).
    ::raise(SIGUSR1);

    csp::shutdown_runtime();
}

#endif // !_WIN32
