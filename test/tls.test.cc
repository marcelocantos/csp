#ifdef CSP_TLS

#include "testutil.h"

#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

using namespace csp;

// --- Helpers ---

static std::string read_file(const char* path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Create a listening socket on localhost, return fd and bound port.
static std::pair<int, uint16_t> listen_localhost() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // OS-assigned
    REQUIRE(0 == ::bind(fd, (struct sockaddr*)&addr, sizeof(addr)));

    socklen_t len = sizeof(addr);
    REQUIRE(0 == ::getsockname(fd, (struct sockaddr*)&addr, &len));

    REQUIRE(0 == ::listen(fd, 5));
    csp::io::set_nonblock(fd);

    return {fd, ntohs(addr.sin_port)};
}

static int connect_localhost(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    csp::io::set_nonblock(fd);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    csp::io::connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    return fd;
}

// --- Tests ---

TEST_CASE("TLS - Handshake and data roundtrip") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto ca_pem     = read_file("test/certs/ca.crt");
    auto cert_pem   = read_file("test/certs/server.crt");
    auto key_pem    = read_file("test/certs/server.key");

    auto listen_pair = listen_localhost();
    int listen_fd = listen_pair.first;
    uint16_t port = listen_pair.second;

    std::atomic<bool> done{false};
    std::string client_received;

    // Server imp
    csp::spawn([&, listen_fd] {
        int client_fd = csp::io::accept(listen_fd, nullptr, nullptr);
        REQUIRE(client_fd >= 0);
        csp::io::set_nonblock(client_fd);

        csp::tls::context ctx(csp::tls::context::server);
        ctx.load_cert(cert_pem.c_str(), cert_pem.size() + 1,
                      key_pem.c_str(), key_pem.size() + 1);

        csp::tls::conn c(ctx, client_fd);
        c.handshake();

        // Read "ping"
        char buf[16]{};
        ssize_t n = c.read(buf, sizeof(buf));
        CHECK(4 == n);
        CHECK("ping" == std::string(buf, n));

        // Write "pong"
        c.write("pong", 4);
        c.shutdown();
        ::close(client_fd);
        ::close(listen_fd);
    });

    // Client imp
    csp::spawn([&, port] {
        int fd = connect_localhost(port);

        csp::tls::context ctx(csp::tls::context::client);
        ctx.load_ca(ca_pem.c_str(), ca_pem.size() + 1);

        csp::tls::conn c(ctx, fd);
        c.set_hostname("localhost");
        c.handshake();

        c.write("ping", 4);

        char buf[16]{};
        ssize_t n = c.read(buf, sizeof(buf));
        client_received = std::string(buf, n);

        c.shutdown();
        ::close(fd);
        done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK("pong" == client_received);
    csp::shutdown_runtime();
}

TEST_CASE("TLS - Cancel during handshake") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    // Listen but never accept — client handshake will block forever.
    auto listen_pair = listen_localhost();
    int listen_fd = listen_pair.first;
    uint16_t port = listen_pair.second;

    std::atomic<bool> got_cancel{false};

    csp::spawn([&, port] {
        auto cancel = csp::cancellation(std::chrono::milliseconds(50));
        int fd = connect_localhost(port);

        auto ca_pem = read_file("test/certs/ca.crt");
        csp::tls::context ctx(csp::tls::context::client);
        ctx.load_ca(ca_pem.c_str(), ca_pem.size() + 1);

        csp::tls::conn c(ctx, fd);
        c.set_hostname("localhost");

        try {
            c.handshake();
        } catch (const csp::canceled&) {
            got_cancel.store(true, std::memory_order_relaxed);
        }
        ::close(fd);
    });

    csp::spawn([listen_fd] {
        // Keep listen_fd alive until the test completes.
        csp::sleep(std::chrono::milliseconds(200));
        ::close(listen_fd);
    });

    csp::schedule();
    CHECK(got_cancel.load());
    csp::shutdown_runtime();
}

TEST_CASE("TLS - Cancel during read") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto ca_pem   = read_file("test/certs/ca.crt");
    auto cert_pem = read_file("test/certs/server.crt");
    auto key_pem  = read_file("test/certs/server.key");

    auto listen_pair = listen_localhost();
    int listen_fd = listen_pair.first;
    uint16_t port = listen_pair.second;

    std::atomic<bool> got_timeout{false};

    // Server: handshake then go silent.
    csp::spawn([&, listen_fd] {
        int client_fd = csp::io::accept(listen_fd, nullptr, nullptr);
        REQUIRE(client_fd >= 0);
        csp::io::set_nonblock(client_fd);

        csp::tls::context ctx(csp::tls::context::server);
        ctx.load_cert(cert_pem.c_str(), cert_pem.size() + 1,
                      key_pem.c_str(), key_pem.size() + 1);

        csp::tls::conn c(ctx, client_fd);
        c.handshake();

        // Go silent — let client timeout.
        csp::sleep(std::chrono::milliseconds(500));
        ::close(client_fd);
        ::close(listen_fd);
    });

    // Client: handshake, then read with timeout.
    csp::spawn([&, port] {
        int fd = connect_localhost(port);

        csp::tls::context ctx(csp::tls::context::client);
        ctx.load_ca(ca_pem.c_str(), ca_pem.size() + 1);

        csp::tls::conn c(ctx, fd);
        c.set_hostname("localhost");
        c.handshake();

        auto cancel = csp::cancellation(std::chrono::milliseconds(50));

        try {
            char buf[16];
            c.read(buf, sizeof(buf));
        } catch (const csp::timed_out&) {
            got_timeout.store(true, std::memory_order_relaxed);
        }
        ::close(fd);
    });

    csp::schedule();
    CHECK(got_timeout.load());
    csp::shutdown_runtime();
}

TEST_CASE("TLS - Invalid cert rejection") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto cert_pem = read_file("test/certs/server.crt");
    auto key_pem  = read_file("test/certs/server.key");

    auto listen_pair = listen_localhost();
    int listen_fd = listen_pair.first;
    uint16_t port = listen_pair.second;

    std::atomic<bool> got_error{false};

    // Server
    csp::spawn([&, listen_fd] {
        int client_fd = csp::io::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) { ::close(listen_fd); return; }
        csp::io::set_nonblock(client_fd);

        csp::tls::context ctx(csp::tls::context::server);
        ctx.load_cert(cert_pem.c_str(), cert_pem.size() + 1,
                      key_pem.c_str(), key_pem.size() + 1);

        csp::tls::conn c(ctx, client_fd);
        try { c.handshake(); } catch (...) {}
        ::close(client_fd);
        ::close(listen_fd);
    });

    // Client with NO CA loaded — verification should fail.
    csp::spawn([&, port] {
        int fd = connect_localhost(port);

        csp::tls::context ctx(csp::tls::context::client);
        // No load_ca — can't verify server cert.

        csp::tls::conn c(ctx, fd);
        c.set_hostname("localhost");

        try {
            c.handshake();
        } catch (const csp::tls::error&) {
            got_error.store(true, std::memory_order_relaxed);
        }
        ::close(fd);
    });

    csp::schedule();
    CHECK(got_error.load());
    csp::shutdown_runtime();
}

TEST_CASE("TLS - Hostname mismatch") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto ca_pem   = read_file("test/certs/ca.crt");
    auto cert_pem = read_file("test/certs/server.crt");
    auto key_pem  = read_file("test/certs/server.key");

    auto listen_pair = listen_localhost();
    int listen_fd = listen_pair.first;
    uint16_t port = listen_pair.second;

    std::atomic<bool> got_error{false};

    // Server
    csp::spawn([&, listen_fd] {
        int client_fd = csp::io::accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) { ::close(listen_fd); return; }
        csp::io::set_nonblock(client_fd);

        csp::tls::context ctx(csp::tls::context::server);
        ctx.load_cert(cert_pem.c_str(), cert_pem.size() + 1,
                      key_pem.c_str(), key_pem.size() + 1);

        csp::tls::conn c(ctx, client_fd);
        try { c.handshake(); } catch (...) {}
        ::close(client_fd);
        ::close(listen_fd);
    });

    // Client with wrong hostname.
    csp::spawn([&, port] {
        int fd = connect_localhost(port);

        csp::tls::context ctx(csp::tls::context::client);
        ctx.load_ca(ca_pem.c_str(), ca_pem.size() + 1);

        csp::tls::conn c(ctx, fd);
        c.set_hostname("wrong.example.com");

        try {
            c.handshake();
        } catch (const csp::tls::error&) {
            got_error.store(true, std::memory_order_relaxed);
        }
        ::close(fd);
    });

    csp::schedule();
    CHECK(got_error.load());
    csp::shutdown_runtime();
}

TEST_CASE("TLS - Concurrent connections") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto ca_pem     = read_file("test/certs/ca.crt");
    auto cert_pem   = read_file("test/certs/server.crt");
    auto key_pem    = read_file("test/certs/server.key");

    auto listen_pair = listen_localhost();
    int listen_fd = listen_pair.first;
    uint16_t port = listen_pair.second;

    constexpr int N = 4;
    std::atomic<int> server_done{0};
    std::atomic<int> client_done{0};

    // Server imp: accept N connections, echo back what each sends.
    csp::spawn([&, listen_fd] {
        for (int i = 0; i < N; ++i) {
            csp::spawn([&, listen_fd] {
                int client_fd = csp::io::accept(listen_fd, nullptr, nullptr);
                REQUIRE(client_fd >= 0);
                csp::io::set_nonblock(client_fd);

                csp::tls::context ctx(csp::tls::context::server);
                ctx.load_cert(cert_pem.c_str(), cert_pem.size() + 1,
                              key_pem.c_str(), key_pem.size() + 1);

                csp::tls::conn c(ctx, client_fd);
                c.handshake();

                char buf[64]{};
                ssize_t n = c.read(buf, sizeof(buf));
                CHECK(n > 0);

                c.write(buf, n);
                c.shutdown();
                ::close(client_fd);
                server_done.fetch_add(1, std::memory_order_relaxed);
            });
        }
    });

    // Client imps: each connects, sends a unique message, verifies echo.
    for (int i = 0; i < N; ++i) {
        csp::spawn([&, port, i] {
            int fd = connect_localhost(port);

            csp::tls::context ctx(csp::tls::context::client);
            ctx.load_ca(ca_pem.c_str(), ca_pem.size() + 1);

            csp::tls::conn c(ctx, fd);
            c.set_hostname("localhost");
            c.handshake();

            std::string msg = "hello-" + std::to_string(i);
            c.write(msg.data(), msg.size());

            char buf[64]{};
            ssize_t n = c.read(buf, sizeof(buf));
            CHECK(msg == std::string(buf, n));

            c.shutdown();
            ::close(fd);
            client_done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Cleanup listener after all connections complete.
    csp::spawn([&, listen_fd] {
        while (server_done.load(std::memory_order_relaxed) < N ||
               client_done.load(std::memory_order_relaxed) < N) {
            csp::sleep(std::chrono::milliseconds(10));
        }
        ::close(listen_fd);
    });

    csp::schedule();
    CHECK(N == server_done.load());
    CHECK(N == client_done.load());
    csp::shutdown_runtime();
}

TEST_CASE("TLS - Large transfer") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto ca_pem     = read_file("test/certs/ca.crt");
    auto cert_pem   = read_file("test/certs/server.crt");
    auto key_pem    = read_file("test/certs/server.key");

    auto listen_pair = listen_localhost();
    int listen_fd = listen_pair.first;
    uint16_t port = listen_pair.second;

    // 256 KB payload.
    constexpr size_t PAYLOAD_SIZE = 256 * 1024;
    std::vector<char> payload(PAYLOAD_SIZE);
    for (size_t i = 0; i < PAYLOAD_SIZE; ++i) {
        payload[i] = static_cast<char>(i & 0xFF);
    }

    std::vector<char> received;
    std::atomic<bool> done{false};

    // Server: read all data and store it.
    csp::spawn([&, listen_fd] {
        int client_fd = csp::io::accept(listen_fd, nullptr, nullptr);
        REQUIRE(client_fd >= 0);
        csp::io::set_nonblock(client_fd);

        csp::tls::context ctx(csp::tls::context::server);
        ctx.load_cert(cert_pem.c_str(), cert_pem.size() + 1,
                      key_pem.c_str(), key_pem.size() + 1);

        csp::tls::conn c(ctx, client_fd);
        c.handshake();

        char buf[4096];
        for (;;) {
            ssize_t n = c.read(buf, sizeof(buf));
            if (n <= 0) break;
            received.insert(received.end(), buf, buf + n);
        }

        c.shutdown();
        ::close(client_fd);
        ::close(listen_fd);
        done.store(true, std::memory_order_relaxed);
    });

    // Client: send entire payload.
    csp::spawn([&, port] {
        int fd = connect_localhost(port);

        csp::tls::context ctx(csp::tls::context::client);
        ctx.load_ca(ca_pem.c_str(), ca_pem.size() + 1);

        csp::tls::conn c(ctx, fd);
        c.set_hostname("localhost");
        c.handshake();

        c.write(payload.data(), payload.size());
        c.shutdown();
        ::close(fd);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK(PAYLOAD_SIZE == received.size());
    CHECK(payload == received);
    csp::shutdown_runtime();
}

TEST_CASE("TLS - conn move semantics") {
    csp::shutdown_runtime();
    csp::set_maxprocs(2);

    auto ca_pem     = read_file("test/certs/ca.crt");
    auto cert_pem   = read_file("test/certs/server.crt");
    auto key_pem    = read_file("test/certs/server.key");

    auto listen_pair = listen_localhost();
    int listen_fd = listen_pair.first;
    uint16_t port = listen_pair.second;

    std::atomic<bool> done{false};
    std::string client_received;

    // Server
    csp::spawn([&, listen_fd] {
        int client_fd = csp::io::accept(listen_fd, nullptr, nullptr);
        REQUIRE(client_fd >= 0);
        csp::io::set_nonblock(client_fd);

        csp::tls::context ctx(csp::tls::context::server);
        ctx.load_cert(cert_pem.c_str(), cert_pem.size() + 1,
                      key_pem.c_str(), key_pem.size() + 1);

        // Create conn, then move it.
        csp::tls::conn c1(ctx, client_fd);
        c1.handshake();

        csp::tls::conn c2(std::move(c1));
        // c1 is now moved-from — its destructor must not crash.

        // Use the moved-to conn for I/O.
        char buf[16]{};
        ssize_t n = c2.read(buf, sizeof(buf));
        CHECK(4 == n);
        CHECK("ping" == std::string(buf, n));

        c2.write("pong", 4);
        c2.shutdown();
        ::close(client_fd);
        ::close(listen_fd);
    });

    // Client
    csp::spawn([&, port] {
        int fd = connect_localhost(port);

        csp::tls::context ctx(csp::tls::context::client);
        ctx.load_ca(ca_pem.c_str(), ca_pem.size() + 1);

        csp::tls::conn c(ctx, fd);
        c.set_hostname("localhost");
        c.handshake();

        // Move via move-assignment.
        csp::tls::conn c2 = std::move(c);

        c2.write("ping", 4);

        char buf[16]{};
        ssize_t n = c2.read(buf, sizeof(buf));
        client_received = std::string(buf, n);

        c2.shutdown();
        ::close(fd);
        done.store(true, std::memory_order_relaxed);
    });

    csp::schedule();
    CHECK(done.load());
    CHECK("pong" == client_received);
    csp::shutdown_runtime();
}

#endif // CSP_TLS
