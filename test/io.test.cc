#include "testutil.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

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
inline std::pair<csp::io::fd_t, csp::io::fd_t> test_pipe() {
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

    return {csp::io::fd_t(reader_sock), csp::io::fd_t(writer_sock)};
}

inline void test_close(csp::io::fd_t fd) { closesocket(fd.raw()); }

inline ssize_t test_raw_write(csp::io::fd_t fd, const void* buf, size_t len) {
    return ::send(fd.raw(), static_cast<const char*>(buf), static_cast<int>(len), 0);
}

inline ssize_t test_raw_read(csp::io::fd_t fd, void* buf, size_t len) {
    return ::recv(fd.raw(), static_cast<char*>(buf), static_cast<int>(len), 0);
}

#else // !_WIN32

inline std::pair<csp::io::fd_t, csp::io::fd_t> test_pipe() {
    int pipefd[2];
    REQUIRE_EQ(0, pipe(pipefd));
    auto rfd = csp::io::fd_t(pipefd[0]);
    auto wfd = csp::io::fd_t(pipefd[1]);
    csp::io::set_nonblock(rfd);
    csp::io::set_nonblock(wfd);
    return {rfd, wfd};
}

inline void test_close(csp::io::fd_t fd) { ::close(fd.raw()); }

inline ssize_t test_raw_write(csp::io::fd_t fd, const void* buf, size_t len) {
    return ::write(fd.raw(), buf, len);
}

inline ssize_t test_raw_read(csp::io::fd_t fd, void* buf, size_t len) {
    return ::read(fd.raw(), buf, len);
}

#endif // _WIN32

// --- Layer 1: wait_readable / wait_writable ---

TEST_CASE("IO---WaitReadable") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = test_pipe();
    csp::io::set_nonblock(rfd);
    csp::io::set_nonblock(wfd);

    std::atomic<bool> got_data{false};

    csp::spawn([&got_data, rfd] {
        csp::io::wait_readable(rfd);
        char buf[16];
        ssize_t n = test_raw_read(rfd, buf, sizeof(buf));
        CHECK(n > 0);
        CHECK('X' == buf[0]);
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

TEST_CASE("IO---WaitWritable") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = test_pipe();
    csp::io::set_nonblock(rfd);
    csp::io::set_nonblock(wfd);

    std::atomic<bool> write_completed{false};

    // Write a byte — should succeed immediately.
    csp::spawn([&write_completed, wfd] {
        char c = 'Y';
        csp::io::wait_writable(wfd);
        ssize_t n = test_raw_write(wfd, &c, 1);
        CHECK(1 == n);
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

TEST_CASE("IO---ReadWrite-roundtrip") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = test_pipe();
    csp::io::set_nonblock(rfd);
    csp::io::set_nonblock(wfd);

    const char* msg = "Hello, CSP I/O!";
    size_t msglen = strlen(msg);
    std::vector<char> result(msglen);
    std::atomic<bool> done{false};

    csp::spawn([wfd, msg, msglen] {
        ssize_t n = csp::io::write(wfd, msg, msglen);
        CHECK(static_cast<ssize_t>(msglen) == n);
        test_close(wfd);
    });

    csp::spawn([&result, &done, rfd, msglen] {
        size_t total = 0;
        while (total < msglen) {
            ssize_t n = csp::io::read(rfd, result.data() + total, msglen - total);
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }
        CHECK(msglen == total);
        done.store(true, std::memory_order_relaxed);
        test_close(rfd);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK(std::string(msg) == std::string(result.data(), result.size()));
    csp::shutdown_runtime();
}

// --- Layer 3: byte_reader ---

TEST_CASE("IO---ByteReader") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

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
    CHECK("Hello, CSP!" == result);
    csp::shutdown_runtime();
}

// --- Layer 3: byte_writer ---

TEST_CASE("IO---ByteWriter") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

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
            ssize_t n = csp::io::read(rfd, buf + total, sizeof(buf) - total);
            if (n <= 0) break;
            total += n;
        }
        result.assign(buf, buf + total);
        done.store(true, std::memory_order_relaxed);
        test_close(rfd);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK("CSP writes!" == std::string(result.data(), result.size()));
    csp::shutdown_runtime();
}

// --- byte_reader (channel-backed) ---

TEST_CASE("IO---byte_reader-exact-chunk") {
    RunStats rs;
    bytes result;
    bool ok = false;

    csp::run([&]{
        auto [w, r] = chan<bytes>{};
        byte_reader br(std::move(r));

        rs.spawn([w = std::move(w)] {
            w << bytes{'A', 'B', 'C', 'D'};
        });

        bytes buf(4);
        ok = (4 == br.read(buf));
        result = buf;
    });

    CHECK(ok);
    CHECK(bytes({'A', 'B', 'C', 'D'}) == result);
}

TEST_CASE("IO---byte_reader-spans-chunks") {
    RunStats rs;
    bytes result1, result2;
    bool ok1 = false, ok2 = false;

    csp::run([&]{
        auto [w, r] = chan<bytes>{};
        byte_reader br(std::move(r));

        rs.spawn([w = std::move(w)] {
            w << bytes{'A', 'B'};
            w << bytes{'C', 'D'};
            w << bytes{'E', 'F'};
        });

        bytes buf(5);
        ok1 = (5 == br.read(buf));
        result1 = buf;

        // Leftover 'F' from previous read.
        bytes buf2(1);
        ok2 = (1 == br.read(buf2));
        result2 = buf2;
    });

    CHECK(ok1);
    CHECK(bytes({'A', 'B', 'C', 'D', 'E'}) == result1);
    CHECK(ok2);
    CHECK(bytes({'F'}) == result2);
}

TEST_CASE("IO---byte_reader-partial-on-close") {
    RunStats rs;
    bytes result;
    int n = -1;

    csp::run([&]{
        auto [w, r] = chan<bytes>{};
        byte_reader br(std::move(r));

        rs.spawn([w = std::move(w)] {
            w << bytes{'X', 'Y'};
        });

        bytes buf(10);
        n = static_cast<int>(br.read(buf));
        result = buf;
    });

    CHECK(2 == n);
    CHECK('X' == result[0]);
    CHECK('Y' == result[1]);
}

TEST_CASE("IO---byte_reader-empty-buffer") {
    RunStats rs;
    int n = -1;

    csp::run([&]{
        auto [w, r] = chan<bytes>{};
        byte_reader br(std::move(r));

        bytes buf;
        n = static_cast<int>(br.read(buf));
    });

    CHECK(0 == n);
}

TEST_CASE("IO---byte_reader-multiple-reads-with-leftover") {
    RunStats rs;
    bytes r0, r1, r2, r3;
    int n0 = -1, n1 = -1, n2 = -1, n3 = -1;

    csp::run([&]{
        auto [w, r] = chan<bytes>{};
        byte_reader br(std::move(r));

        rs.spawn([w = std::move(w)] {
            // Send 10 bytes in one chunk.
            w << bytes{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
        });

        bytes buf(3);

        n0 = static_cast<int>(br.read(buf)); r0 = buf;
        n1 = static_cast<int>(br.read(buf)); r1 = buf;
        n2 = static_cast<int>(br.read(buf)); r2 = buf;

        // Only 1 byte left.
        n3 = static_cast<int>(br.read(buf)); r3 = buf;
    });

    CHECK(3 == n0);
    CHECK(bytes({'0', '1', '2'}) == r0);

    CHECK(3 == n1);
    CHECK(bytes({'3', '4', '5'}) == r1);

    CHECK(3 == n2);
    CHECK(bytes({'6', '7', '8'}) == r2);

    CHECK(1 == n3);
    CHECK('9' == r3[0]);
}

TEST_CASE("IO---byte_reader-closed-reader-returns-zero") {
    RunStats rs;
    int n = -1;

    csp::run([&]{
        auto [w, r] = chan<bytes>{};
        byte_reader br(std::move(r));

        // Close the writer immediately.
        { auto _ = std::move(w); }

        bytes buf(4);
        n = static_cast<int>(br.read(buf));
    });

    CHECK(0 == n);
}

// --- Layer 3: split_lines — pure channel test, no I/O ---

TEST_CASE("IO---Lines-framing") {
    RunStats rs;
    std::string l0, l1, l2;

    csp::run([&]{
        auto [w, r] = chan<bytes>{};
        auto lr = part::io::split_lines.spawn(std::move(r));

        rs.spawn([w = std::move(w)] {
            std::string data = "hello\nworld\nfoo\n";
            bytes v(data.begin(), data.end());
            w << std::move(v);
        });

        l0 = lr.read();
        l1 = lr.read();
        l2 = lr.read();
    });

    CHECK("hello" == l0);
    CHECK("world" == l1);
    CHECK("foo" == l2);
}

TEST_CASE("IO---Lines-partial-flush") {
    RunStats rs;
    std::string l0, l1;
    bool closed = false;

    csp::run([&]{
        auto [w, r] = chan<bytes>{};
        auto lr = part::io::split_lines.spawn(std::move(r));

        rs.spawn([w = std::move(w)] {
            std::string data = "hello\nworld";
            bytes v(data.begin(), data.end());
            w << std::move(v);
        });

        l0 = lr.read();
        l1 = lr.read();  // partial line flushed on input close
        std::string _;
        closed = !bool(lr >> _);
    });

    CHECK("hello" == l0);
    CHECK("world" == l1);
    CHECK(closed);
}

TEST_CASE("IO---Lines-multi-chunk") {
    RunStats rs;
    std::string l0, l1;
    bool closed = false;

    csp::run([&]{
        auto [w, r] = chan<bytes>{};
        auto lr = part::io::split_lines.spawn(std::move(r));

        rs.spawn([w = std::move(w)] {
            // Split "hello\nworld\n" across two chunks.
            std::string c1 = "hel";
            std::string c2 = "lo\nworld\n";
            w << bytes(c1.begin(), c1.end());
            w << bytes(c2.begin(), c2.end());
        });

        l0 = lr.read();
        l1 = lr.read();
        std::string _;
        closed = !bool(lr >> _);
    });

    CHECK("hello" == l0);
    CHECK("world" == l1);
    CHECK(closed);
}

// --- Layer 3: fixed_frames() — pure channel test ---

TEST_CASE("IO---Fixed-framing") {
    RunStats rs;
    bytes f1, f2;
    bool closed = false;

    csp::run([&]{
        auto [w, r] = chan<bytes>{};
        auto fr = part::io::fixed_frames(4).spawn(std::move(r));

        rs.spawn([w = std::move(w)] {
            // 10 bytes → 2 full frames of 4, partial 2 discarded.
            std::string data = "AABBCCDDEE";
            w << bytes(data.begin(), data.end());
        });

        f1 = fr.read();
        f2 = fr.read();

        // Partial "EE" is discarded.
        bytes _;
        closed = !bool(fr >> _);
    });

    CHECK(4 == f1.size());
    CHECK('A' == f1[0]); CHECK('A' == f1[1]);
    CHECK('B' == f1[2]); CHECK('B' == f1[3]);

    CHECK(4 == f2.size());
    CHECK('C' == f2[0]); CHECK('C' == f2[1]);
    CHECK('D' == f2[2]); CHECK('D' == f2[3]);

    CHECK(closed);
}

TEST_CASE("IO---Fixed-multi-chunk") {
    RunStats rs;
    bytes f1;
    bool closed = false;

    csp::run([&]{
        auto [w, r] = chan<bytes>{};
        auto fr = part::io::fixed_frames(4).spawn(std::move(r));

        rs.spawn([w = std::move(w)] {
            // Frame boundary spans chunks.
            w << bytes{'A', 'B'};
            w << bytes{'C', 'D', 'E', 'F'};
        });

        f1 = fr.read();

        // Partial "EF" is discarded.
        bytes _;
        closed = !bool(fr >> _);
    });

    CHECK(4 == f1.size());
    CHECK('A' == f1[0]); CHECK('B' == f1[1]);
    CHECK('C' == f1[2]); CHECK('D' == f1[3]);

    CHECK(closed);
}

// --- io::read_all ---

TEST_CASE("IO---ReadAll") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = test_pipe();

    csp::spawn([wfd] {
        const char* msg = "All the bytes!";
        (void)csp::io::write(wfd, msg, strlen(msg));
        test_close(wfd);
    });

    bytes result;
    std::atomic<bool> done{false};

    csp::spawn([&result, &done, rfd] {
        result = csp::io::read_all(rfd);
        done.store(true, std::memory_order_relaxed);
        test_close(rfd);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK("All the bytes!" == std::string(result.begin(), result.end()));
    csp::shutdown_runtime();
}

// --- io::write_all ---

TEST_CASE("IO---WriteAll") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = test_pipe();

    csp::spawn([wfd] {
        std::string msg = "Write all bytes!";
        bytes data(msg.begin(), msg.end());
        csp::io::write_all(wfd, data);
        test_close(wfd);
    });

    bytes result;
    std::atomic<bool> done{false};

    csp::spawn([&result, &done, rfd] {
        result = csp::io::read_all(rfd);
        done.store(true, std::memory_order_relaxed);
        test_close(rfd);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK("Write all bytes!" == std::string(result.begin(), result.end()));
    csp::shutdown_runtime();
}

// --- io::lines (convenience) ---

TEST_CASE("IO---Lines-convenience") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto [rfd, wfd] = test_pipe();

    csp::spawn([wfd] {
        const char* data = "alpha\nbeta\ngamma\n";
        (void)csp::io::write(wfd, data, strlen(data));
        test_close(wfd);
    });

    std::vector<std::string> result;
    std::atomic<bool> done{false};

    csp::spawn([&result, &done, rfd] {
        auto lr = part::io::lines(rfd);
        for (std::string line; lr >> line;) {
            result.push_back(std::move(line));
        }
        done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(done.load());
    REQUIRE(3 == result.size());
    CHECK("alpha" == result[0]);
    CHECK("beta" == result[1]);
    CHECK("gamma" == result[2]);
    csp::shutdown_runtime();
}

// --- file::read / file::write ---

TEST_CASE("IO---File-roundtrip") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto tmp = std::filesystem::temp_directory_path() / "csp_test_file_roundtrip.bin";
    std::string path = tmp.string();
    std::string content = "Hello from file I/O!";
    bytes data(content.begin(), content.end());
    bytes result;
    std::atomic<bool> done{false};

    csp::spawn([&] {
        csp::file::write(path, data);
        result = csp::file::read(path);
        done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK(data == result);
    std::filesystem::remove(path);
    csp::shutdown_runtime();
}

// --- Composed pipeline: split_lines(byte_reader(fd)) ---

TEST_CASE("IO---Composed-lines-from-pipe") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

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
    REQUIRE(3 == result.size());
    CHECK("alpha" == result[0]);
    CHECK("beta" == result[1]);
    CHECK("gamma" == result[2]);
    csp::shutdown_runtime();
}

// --- Multiple concurrent I/O waiters ---

TEST_CASE("IO---Multiple-concurrent-waiters") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int N = 8;
    csp::io::fd_t rfds[N], wfds[N];
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
    CHECK(N == count.load());
    csp::shutdown_runtime();
}

// --- csp::blocking ---

TEST_CASE("IO---Blocking-offload") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    std::atomic<bool> ran{false};
    std::atomic<std::thread::id> pool_tid{};

    csp::spawn([&] {
        auto main_tid = std::this_thread::get_id();
        int result = csp::blocking([&] {
            pool_tid.store(std::this_thread::get_id(), std::memory_order_relaxed);
            ran.store(true, std::memory_order_relaxed);
            return 42;
        });
        CHECK(42 == result);
        // The blocking lambda ran on a different thread.
        CHECK(main_tid != pool_tid.load());
    });

    csp::schedule();
    CHECK(ran.load());
    csp::shutdown_runtime();
}

// --- DNS resolution ---

TEST_CASE("IO---Resolve-unresolvable-hostname") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

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

TEST_CASE("IO---Resolve-localhost") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

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

TEST_CASE("IO---Blocking-void-return") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

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

TEST_CASE("IO---Blocking-with-non-trivial-return-type") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    std::atomic<bool> done{false};

    csp::spawn([&] {
        std::string result = csp::blocking([] {
            return std::string("hello from pool");
        });
        CHECK("hello from pool" == result);
        done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(done.load());
    csp::shutdown_runtime();
}

TEST_CASE("IO---Multiple-concurrent-blocking-calls") {
    csp::shutdown_runtime();
    csp::set_maxprocs(4);

    constexpr int N = 5;
    std::atomic<int> completed{0};

    for (int i = 0; i < N; ++i) {
        csp::spawn([&completed, i] {
            int result = csp::blocking([i] {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return i * 10;
            });
            CHECK(i * 10 == result);
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    csp::schedule();
    CHECK(N == completed.load());
    csp::shutdown_runtime();
}

// --- Console signal channels (Windows only) ---

#ifdef _WIN32

TEST_CASE("IO---Console-signal-delivery") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto sig = csp::win::signal::notify({CTRL_C_EVENT});
    std::atomic<bool> got_signal{false};

    csp::spawn([&got_signal, sig = std::move(sig)] {
        DWORD ev;
        sig >> ev;
        CHECK_EQ(CTRL_C_EVENT, ev);
        got_signal.store(true, std::memory_order_relaxed);
    });

    csp::spawn([] {
        csp::sleep(std::chrono::milliseconds(10));
        // Use raise() instead of GenerateConsoleCtrlEvent() — the latter
        // sends to the entire process group and kills CI runners.
        csp::win::signal::raise(CTRL_C_EVENT);
    });

    csp::schedule();
    CHECK(got_signal.load());
    csp::shutdown_runtime();
}

TEST_CASE("IO---Console-signal-multiple-events") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto sig =
        csp::win::signal::notify({CTRL_C_EVENT, CTRL_BREAK_EVENT});
    std::vector<DWORD> received;
    std::atomic<bool> done{false};

    csp::spawn([&received, &done, sig = std::move(sig)] {
        DWORD ev;
        sig >> ev;
        received.push_back(ev);
        sig >> ev;
        received.push_back(ev);
        done.store(true, std::memory_order_relaxed);
    });

    csp::spawn([] {
        csp::sleep(std::chrono::milliseconds(10));
        csp::win::signal::raise(CTRL_C_EVENT);
        csp::sleep(std::chrono::milliseconds(10));
        csp::win::signal::raise(CTRL_BREAK_EVENT);
    });

    csp::schedule();
    CHECK(done.load());
    REQUIRE_EQ(2, received.size());
    CHECK_EQ(CTRL_C_EVENT, received[0]);
    CHECK_EQ(CTRL_BREAK_EVENT, received[1]);
    csp::shutdown_runtime();
}

TEST_CASE("IO---Console-signal-reader-drop-cleanup") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    {
        auto sig = csp::win::signal::notify({CTRL_C_EVENT});
        // Drop immediately — sentinel closes the socket, imps exit.
    }

    // Wait for producer + sentinel imps to finish.
    csp::schedule();

    // Event after cleanup — handler skips (mask cleared).
    csp::win::signal::raise(CTRL_C_EVENT);

    csp::shutdown_runtime();
}

#endif // _WIN32

// --- Signal channels (Unix only) ---

#ifndef _WIN32

TEST_CASE("IO---Signal-delivery") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto sig = csp::signal::notify({SIGUSR1});
    std::atomic<bool> got_signal{false};

    csp::spawn([&got_signal, sig = std::move(sig)] {
        int s;
        sig >> s;
        CHECK(SIGUSR1 == s);
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

TEST_CASE("IO---Signal-multiple-signals") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

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
    REQUIRE(2 == received.size());
    CHECK(SIGUSR1 == received[0]);
    CHECK(SIGUSR2 == received[1]);
    csp::shutdown_runtime();
}

TEST_CASE("IO---Signal-reader-drop-cleanup") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

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
