#pragma once

#include <csp/csp.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#ifndef _WIN32
#include <thread>
#include <unordered_map>
#endif

namespace csp::detail {

struct Imp;

#ifdef _WIN32

// Windows stub — Phase 2 will add RegisterWaitForSingleObject-based
// reactor. For now, has_pending_signals() returns false so the default
// scheduler exits when there is no more local work. Timer and fd
// creation methods assert — they should not be called until Phase 2.

class Reactor {
public:
    static Reactor& instance();

    std::pair<reader<>, uintptr_t> create_timer(int64_t delay_ns);
    void cancel_timer(uintptr_t ident);

    void ensure_started();

    bool has_pending_signals() const { return false; }

    void shutdown();

private:
    Reactor() = default;
    Reactor(Reactor const&) = delete;
    Reactor& operator=(Reactor const&) = delete;
};

#else // !_WIN32

class Reactor {
public:
    static Reactor& instance();

    // --- Signal-based API (new) ---
    // These create kqueue events whose firing drops a writer<>,
    // producing a death signal observable via prialt(~reader).

    // Create a one-shot timer. Returns (reader, ident).
    // Caller wraps in timer_signal for RAII cancellation.
    std::pair<reader<>, uintptr_t> create_timer(int64_t delay_ns);

    // Create a one-shot fd readiness event. Returns (reader, filter).
    // Caller wraps in fd_signal for RAII cancellation.
    reader<> create_fd_event(int fd, int16_t filter);

    // Cancel a timer by ident. EV_DELETE + erase writer.
    // No-op if already fired.
    void cancel_timer(uintptr_t ident);

    // Cancel an fd event. EV_DELETE + erase writer.
    // No-op if already fired.
    void cancel_fd(int fd, int16_t filter);

    // Lazy init: creates kqueue fd and spawns reactor thread on first call.
    void ensure_started();

    // True if there are pending reactor signals (timers or fd events)
    // that haven't fired yet.
    bool has_pending_signals() const {
        return pending_signals_.load(std::memory_order_acquire) > 0;
    }

    // Stop reactor thread and close kqueue fd. Idempotent.
    void shutdown();

private:
    Reactor() = default;
    Reactor(Reactor const&) = delete;
    Reactor& operator=(Reactor const&) = delete;

    void loop();
    void wake();
    void fire_signal(uintptr_t ident, int16_t filter);

    int kq_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::mutex start_mu_;

    // Protects writer maps. Lock ordering: signal_mu_ before any
    // channel lock (channel_mu_) and before global_mu / run_mu.
    std::mutex signal_mu_;

    // Monotonic ident generator for timer events.
    std::atomic<uintptr_t> next_ident_{1};

    // Number of pending signals (created but not yet fired/cancelled).
    std::atomic<int> pending_signals_{0};

    // Writer endpoints keyed by event identity.
    // When the reactor loop fires an event, it erases the writer;
    // the writer destructor triggers endpoint death on the channel,
    // waking any imp in prialt watching ~reader on the same channel.
    std::unordered_map<uintptr_t, writer<>> timer_writers_;
    std::unordered_map<int, writer<>>       read_writers_;
    std::unordered_map<int, writer<>>       write_writers_;
};

#endif // _WIN32

}
