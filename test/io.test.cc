#include "testutil.h"

#include <atomic>
#include <csignal>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace csp;
using namespace csp::part;

// --- Layer 1: wait_readable / wait_writable ---

TEST_CASE("IO - WaitReadable") {
    csp::init_runtime(2);

    int pipefd[2];
    REQUIRE_EQ(0, pipe(pipefd));
    io::set_nonblock(pipefd[0]);
    io::set_nonblock(pipefd[1]);

    std::atomic<bool> got_data{false};

    csp::spawn([&got_data, rfd = pipefd[0]] {
        io::wait_readable(rfd);
        char buf[16];
        ssize_t n = ::read(rfd, buf, sizeof(buf));
        CHECK_GT(n, 0);
        CHECK_EQ('X', buf[0]);
        got_data.store(true, std::memory_order_relaxed);
        ::close(rfd);
    });

    csp::spawn([wfd = pipefd[1]] {
        csp::sleep(std::chrono::milliseconds(10));
        ::write(wfd, "X", 1);
        ::close(wfd);
    });

    csp::schedule();
    CHECK(got_data.load());
    csp::shutdown_runtime();
}

TEST_CASE("IO - WaitWritable") {
    csp::init_runtime(2);

    int pipefd[2];
    REQUIRE_EQ(0, pipe(pipefd));
    io::set_nonblock(pipefd[0]);
    io::set_nonblock(pipefd[1]);

    std::atomic<bool> write_completed{false};

    // Fill the pipe buffer to force EAGAIN.
    csp::spawn([&write_completed, wfd = pipefd[1]] {
        // Write a byte — should succeed immediately.
        char c = 'Y';
        io::wait_writable(wfd);
        ssize_t n = ::write(wfd, &c, 1);
        CHECK_EQ(1, n);
        write_completed.store(true, std::memory_order_relaxed);
        ::close(wfd);
    });

    csp::spawn([rfd = pipefd[0]] {
        csp::sleep(std::chrono::milliseconds(10));
        char buf[16];
        ::read(rfd, buf, sizeof(buf));
        ::close(rfd);
    });

    csp::schedule();
    CHECK(write_completed.load());
    csp::shutdown_runtime();
}

// --- Layer 2: io::read / io::write ---

TEST_CASE("IO - ReadWrite roundtrip") {
    csp::init_runtime(2);

    int pipefd[2];
    REQUIRE_EQ(0, pipe(pipefd));
    io::set_nonblock(pipefd[0]);
    io::set_nonblock(pipefd[1]);

    const char* msg = "Hello, CSP I/O!";
    size_t msglen = strlen(msg);
    std::vector<char> result(msglen);
    std::atomic<bool> done{false};

    csp::spawn([wfd = pipefd[1], msg, msglen] {
        ssize_t n = io::write(wfd, msg, msglen);
        CHECK_EQ(static_cast<ssize_t>(msglen), n);
        ::close(wfd);
    });

    csp::spawn([&result, &done, rfd = pipefd[0], msglen] {
        size_t total = 0;
        while (total < msglen) {
            ssize_t n = io::read(rfd, result.data() + total, msglen - total);
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }
        CHECK_EQ(msglen, total);
        done.store(true, std::memory_order_relaxed);
        ::close(rfd);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK_EQ(std::string(msg), std::string(result.data(), result.size()));
    csp::shutdown_runtime();
}

// --- Layer 3: byte_reader ---

TEST_CASE("IO - ByteReader") {
    csp::init_runtime(2);

    int pipefd[2];
    REQUIRE_EQ(0, pipe(pipefd));

    // byte_reader owns pipefd[0] and closes it.
    auto r = byte_reader(pipefd[0], 16).spawn();

    std::vector<uint8_t> all;
    std::atomic<bool> done{false};

    csp::spawn([wfd = pipefd[1]] {
        const char* msg = "Hello, CSP!";
        ::write(wfd, msg, strlen(msg));
        ::close(wfd);
    });

    csp::spawn([&all, &done, r = std::move(r)] {
        for (std::vector<uint8_t> chunk; r >> chunk;) {
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

    int pipefd[2];
    REQUIRE_EQ(0, pipe(pipefd));

    // byte_writer owns pipefd[1] and closes it.
    auto w = byte_writer(pipefd[1]).spawn();

    std::atomic<bool> done{false};
    std::vector<char> result;

    csp::spawn([w = std::move(w)] {
        std::string msg = "CSP writes!";
        std::vector<uint8_t> chunk(msg.begin(), msg.end());
        w << std::move(chunk);
    });

    csp::spawn([&result, &done, rfd = pipefd[0]] {
        char buf[64];
        ssize_t total = 0;
        for (;;) {
            ssize_t n = ::read(rfd, buf + total, sizeof(buf) - total);
            if (n <= 0) break;
            total += n;
        }
        result.assign(buf, buf + total);
        done.store(true, std::memory_order_relaxed);
        ::close(rfd);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK_EQ("CSP writes!", std::string(result.data(), result.size()));
    csp::shutdown_runtime();
}

// --- Layer 3: split_lines() — pure channel test, no I/O ---

TEST_CASE("IO - Lines framing") {
    auto [w, r] = chan<std::vector<uint8_t>>{};
    auto lr = split_lines().spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        std::string data = "hello\nworld\nfoo\n";
        std::vector<uint8_t> v(data.begin(), data.end());
        w << std::move(v);
    });

    CHECK_EQ("hello", lr.read());
    CHECK_EQ("world", lr.read());
    CHECK_EQ("foo", lr.read());
}

TEST_CASE("IO - Lines partial flush") {
    auto [w, r] = chan<std::vector<uint8_t>>{};
    auto lr = split_lines().spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        std::string data = "hello\nworld";
        std::vector<uint8_t> v(data.begin(), data.end());
        w << std::move(v);
    });

    CHECK_EQ("hello", lr.read());
    CHECK_EQ("world", lr.read());  // partial line flushed on input close
    std::string _;
    CHECK_FALSE(bool(lr >> _));
}

TEST_CASE("IO - Lines multi-chunk") {
    auto [w, r] = chan<std::vector<uint8_t>>{};
    auto lr = split_lines().spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        // Split "hello\nworld\n" across two chunks.
        std::string c1 = "hel";
        std::string c2 = "lo\nworld\n";
        w << std::vector<uint8_t>(c1.begin(), c1.end());
        w << std::vector<uint8_t>(c2.begin(), c2.end());
    });

    CHECK_EQ("hello", lr.read());
    CHECK_EQ("world", lr.read());
    std::string _;
    CHECK_FALSE(bool(lr >> _));
}

// --- Layer 3: fixed_frames() — pure channel test ---

TEST_CASE("IO - Fixed framing") {
    auto [w, r] = chan<std::vector<uint8_t>>{};
    auto fr = fixed_frames(4).spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        // 10 bytes → 2 full frames of 4, partial 2 discarded.
        std::string data = "AABBCCDDEE";
        w << std::vector<uint8_t>(data.begin(), data.end());
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
    std::vector<uint8_t> _;
    CHECK_FALSE(bool(fr >> _));
}

TEST_CASE("IO - Fixed multi-chunk") {
    auto [w, r] = chan<std::vector<uint8_t>>{};
    auto fr = fixed_frames(4).spawn(std::move(r));

    csp::spawn([w = std::move(w)] {
        // Frame boundary spans chunks.
        w << std::vector<uint8_t>{'A', 'B'};
        w << std::vector<uint8_t>{'C', 'D', 'E', 'F'};
    });

    auto f1 = fr.read();
    CHECK_EQ(4, f1.size());
    CHECK_EQ('A', f1[0]); CHECK_EQ('B', f1[1]);
    CHECK_EQ('C', f1[2]); CHECK_EQ('D', f1[3]);

    // Partial "EF" is discarded.
    std::vector<uint8_t> _;
    CHECK_FALSE(bool(fr >> _));
}

// --- Composed pipeline: split_lines(byte_reader(fd)) ---

TEST_CASE("IO - Composed lines from pipe") {
    csp::init_runtime(2);

    int pipefd[2];
    REQUIRE_EQ(0, pipe(pipefd));

    auto lr = split_lines().spawn(byte_reader(pipefd[0]).spawn());

    std::vector<std::string> result;
    std::atomic<bool> done{false};

    csp::spawn([wfd = pipefd[1]] {
        const char* data = "alpha\nbeta\ngamma\n";
        ::write(wfd, data, strlen(data));
        ::close(wfd);
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
    int pipes[N][2];
    for (int i = 0; i < N; ++i) {
        REQUIRE_EQ(0, pipe(pipes[i]));
        io::set_nonblock(pipes[i][0]);
        io::set_nonblock(pipes[i][1]);
    }

    std::atomic<int> count{0};

    for (int i = 0; i < N; ++i) {
        csp::spawn([&count, rfd = pipes[i][0]] {
            io::wait_readable(rfd);
            char buf[4];
            ::read(rfd, buf, sizeof(buf));
            count.fetch_add(1, std::memory_order_relaxed);
            ::close(rfd);
        });
    }

    // Delay, then write to all pipes.
    csp::spawn([&pipes] {
        csp::sleep(std::chrono::milliseconds(20));
        for (int i = 0; i < N; ++i) {
            ::write(pipes[i][1], "!", 1);
            ::close(pipes[i][1]);
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

TEST_CASE("IO - Resolve localhost") {
    csp::init_runtime(2);

    std::atomic<bool> resolved{false};

    csp::spawn([&] {
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        auto r = io::resolve("localhost", "80", &hints);
        CHECK_EQ(0, r.error);
        CHECK(r.info != nullptr);
        resolved.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(resolved.load());
    csp::shutdown_runtime();
}

// --- Signal channels ---

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
