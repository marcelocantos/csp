#ifdef _WIN32

#include <csp/win/signal.h>
#include <csp/io.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>

// Socket-pair trick for console control event delivery.
//
// Each notify() call creates a loopback TCP socket pair. The console
// control handler sends the event type (as a byte) to every socket pair
// whose mask includes that event. An imp per pair reads bytes and writes
// them to a CSP channel.
//
// The handler runs on a normal OS thread (not an async-signal context),
// so all APIs are safe. We use the same lock-free atomic-guard pattern
// as the Unix self-pipe implementation for consistency and minimal
// overhead.

namespace {

constexpr int MAX_CONSOLE_PIPES = 64;
constexpr DWORD MAX_EVENT = 6;  // CTRL_SHUTDOWN_EVENT

struct ConsolePipe {
    SOCKET write_sock;
    std::atomic<uint32_t> event_mask;  // bit per DWORD event type (0–6)
};

ConsolePipe g_console_pipes[MAX_CONSOLE_PIPES];
std::atomic<int> g_console_pipe_count{0};

std::once_flag g_handler_once;

BOOL WINAPI console_handler(DWORD event) {
    if (event > MAX_EVENT) return FALSE;
    uint32_t bit = 1u << event;
    int count = g_console_pipe_count.load(std::memory_order_acquire);
    bool handled = false;
    for (int i = 0; i < count; ++i) {
        if (g_console_pipes[i].event_mask.load(std::memory_order_acquire) &
            bit) {
            uint8_t byte = static_cast<uint8_t>(event);
            ::send(g_console_pipes[i].write_sock,
                   reinterpret_cast<const char*>(&byte), 1, 0);
            handled = true;
        }
    }
    return handled ? TRUE : FALSE;
}

// Create a connected loopback TCP socket pair.
std::pair<SOCKET, SOCKET> create_socket_pair() {
    // Ensure Winsock is initialized (ref-counted, multiple calls safe).
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener != INVALID_SOCKET);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // ephemeral port
    int rc = ::bind(listener, reinterpret_cast<sockaddr*>(&addr),
                    sizeof(addr));
    assert(rc == 0);
    rc = ::listen(listener, 1);
    assert(rc == 0);

    int addrlen = sizeof(addr);
    rc = ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrlen);
    assert(rc == 0);

    SOCKET writer_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(writer_sock != INVALID_SOCKET);
    rc = ::connect(writer_sock, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr));
    assert(rc == 0);

    SOCKET reader_sock = ::accept(listener, nullptr, nullptr);
    assert(reader_sock != INVALID_SOCKET);
    ::closesocket(listener);

    return {reader_sock, writer_sock};
}

} // anonymous namespace

namespace csp::win::signal {

reader<DWORD> notify(std::initializer_list<DWORD> events) {
    auto [read_sock, write_sock] = create_socket_pair();
    csp::io::set_nonblock(read_sock);

    uint32_t mask = 0;
    for (DWORD ev : events) {
        assert(ev <= MAX_EVENT);
        mask |= 1u << ev;
    }

    int idx = g_console_pipe_count.load(std::memory_order_relaxed);
    assert(idx < MAX_CONSOLE_PIPES);
    g_console_pipes[idx].write_sock = write_sock;
    // Store mask with relaxed — release on count increment pairs with
    // handler's acquire on count.
    g_console_pipes[idx].event_mask.store(mask, std::memory_order_relaxed);
    // Release count — handler acquires this to see write_sock and mask.
    g_console_pipe_count.store(idx + 1, std::memory_order_release);

    // Install handler once. call_once is thread-safe.
    std::call_once(g_handler_once, []() {
        SetConsoleCtrlHandler(console_handler, TRUE);
    });

    SOCKET rsock = read_sock;
    SOCKET wsock = write_sock;
    return spawn_producer<DWORD>([rsock, wsock, idx](writer<DWORD> out) {
        internal::descr("win/signal");

        // Sentinel imp: watches for output reader death or producer exit.
        // Closes the socket write end so the producer's io::read() returns
        // EOF and the loop terminates.
        auto out_copy = out.copy();
        auto [kill_w, kill_r] = chan<>{};
        csp::spawn([out_copy = std::move(out_copy),
                     kill_r = std::move(kill_r), wsock, idx] {
            internal::descr("win/signal/sentinel");
            prialt(~out_copy, ~kill_r);
            g_console_pipes[idx].event_mask.store(
                0, std::memory_order_release);
            ::closesocket(wsock);
        });

        uint8_t buf[32];
        for (;;) {
            ssize_t n = io::read(rsock, buf, sizeof(buf));
            if (n <= 0) break;  // EOF (sentinel closed wsock) or error
            for (ssize_t i = 0; i < n; ++i) {
                if (!(out << static_cast<DWORD>(buf[i]))) {
                    ::closesocket(rsock);
                    return;  // kill_w destroyed → sentinel cleans up wsock
                }
            }
        }
        ::closesocket(rsock);
        // kill_w destroyed → sentinel cleans up wsock
    });
}

} // namespace csp::win::signal

#endif // _WIN32
