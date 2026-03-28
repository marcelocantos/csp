#ifndef _WIN32

#include <csp/signal.h>
#include <csp/io.h>

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>

// Self-pipe trick for async-signal-safe delivery.
//
// Each notify() call creates a pipe. The signal handler writes the
// signal number (as a byte) to every pipe whose mask includes that
// signal. An imp per pipe reads bytes and writes them to a
// CSP channel.
//
// The handler only touches: atomic loads, pipe write() — both
// async-signal-safe. All complex logic lives in the imps.

namespace {

constexpr int MAX_SIG_PIPES = 64;
constexpr int MAX_SIGNO = 63;

struct SigPipe {
    int write_fd;
    std::atomic<uint64_t> sig_mask;  // lock-free atomics are signal-safe
};

SigPipe g_sig_pipes[MAX_SIG_PIPES];
std::atomic<int> g_sig_pipe_count{0};

std::mutex g_sig_mu;  // protects handler installation (not used in handler)
bool g_handler_installed[MAX_SIGNO + 1] = {};
struct sigaction g_old_actions[MAX_SIGNO + 1] = {};

void sig_handler(int sig) {
    if (sig < 0 || sig > MAX_SIGNO) return;
    uint64_t bit = 1ULL << sig;
    uint8_t byte = static_cast<uint8_t>(sig);
    int count = g_sig_pipe_count.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i) {
        if (g_sig_pipes[i].sig_mask.load(std::memory_order_acquire) & bit) {
            ::write(g_sig_pipes[i].write_fd, &byte, 1);
        }
    }
}

void install_handler(int sig) {
    if (g_handler_installed[sig]) return;
    struct sigaction sa{};
    sa.sa_handler = sig_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, &g_old_actions[sig]);
    g_handler_installed[sig] = true;
}

// Close stale write fds at process exit.  By static destruction time,
// all imps are dead and no signal handler can race.
struct SigPipeCleanup {
    ~SigPipeCleanup() {
        int count = g_sig_pipe_count.load(std::memory_order_relaxed);
        for (int i = 0; i < count; ++i) {
            if (g_sig_pipes[i].write_fd >= 0) {
                ::close(g_sig_pipes[i].write_fd);
                g_sig_pipes[i].write_fd = -1;
            }
        }
    }
};
static SigPipeCleanup g_sig_pipe_cleanup;

} // anonymous namespace

namespace csp::signal {

reader<int> notify(std::initializer_list<int> sigs) {
    int pipefd[2];
    int rc = ::pipe(pipefd);
    assert(rc == 0);
    io::set_nonblock(pipefd[0]);
    io::set_nonblock(pipefd[1]);

    // Prevent SIGPIPE when reader is dropped and write end is still
    // referenced by g_sig_pipes. On macOS, F_SETNOSIGPIPE suppresses
    // SIGPIPE for writes to this specific fd.
#ifdef F_SETNOSIGPIPE
    fcntl(pipefd[1], F_SETNOSIGPIPE, 1);
#endif

    uint64_t mask = 0;
    for (int sig : sigs) {
        assert(sig > 0 && sig <= MAX_SIGNO);
        mask |= 1ULL << sig;
    }

    int idx;
    {
        std::lock_guard<std::mutex> lk(g_sig_mu);

        idx = g_sig_pipe_count.load(std::memory_order_relaxed);
        assert(idx < MAX_SIG_PIPES);
        g_sig_pipes[idx].write_fd = pipefd[1];
        // Store mask with release — pairs with handler's acquire on count.
        g_sig_pipes[idx].sig_mask.store(mask, std::memory_order_relaxed);
        // Release count — handler acquires this to see write_fd and mask.
        g_sig_pipe_count.store(idx + 1, std::memory_order_release);

        for (int sig : sigs) {
            install_handler(sig);
        }
    }

    int rfd = pipefd[0];
    int wfd = pipefd[1];
    return spawn_producer<int>([rfd, wfd, idx](writer<int> out) {
        internal::descr("signal");

        // Sentinel imp: watches for output reader death or producer exit.
        // Closes the pipe write end so the producer's io::read() returns
        // EOF and the loop terminates.  The kill channel detects producer
        // exit (kill_w destroyed → ~kill_r fires in sentinel).
        auto out_copy = out.copy();
        auto [kill_w, kill_r] = chan<>{};
        csp::spawn([out_copy = std::move(out_copy),
                     kill_r = std::move(kill_r), wfd, idx] {
            internal::descr("signal/sentinel");
            prialt(~out_copy, ~kill_r);
            // Clear mask so the signal handler stops writing to this pipe.
            g_sig_pipes[idx].sig_mask.store(0, std::memory_order_release);
            // Redirect wfd to /dev/null before closing, so any in-flight
            // handler write() hits /dev/null instead of a reused fd.
            // dup2 is async-signal-safe and atomically replaces the fd.
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, wfd);
                ::close(devnull);
            }
            ::close(wfd);
            g_sig_pipes[idx].write_fd = -1;
        });

        uint8_t buf[32];
        for (;;) {
            ssize_t n = io::read(rfd, buf, sizeof(buf));
            if (n <= 0) break;  // EOF (sentinel closed wfd) or error
            for (ssize_t i = 0; i < n; ++i) {
                if (!(out << static_cast<int>(buf[i]))) {
                    ::close(rfd);
                    return;  // kill_w destroyed → sentinel cleans up wfd
                }
            }
        }
        ::close(rfd);
        // kill_w destroyed → sentinel cleans up wfd
    });
}

} // namespace csp::signal

#endif // !_WIN32
