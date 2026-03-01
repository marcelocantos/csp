#pragma once

#include <csp/csp.h>
#include <csp/internal/signal.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#ifndef _WIN32
#include <thread>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#endif

namespace csp::detail {

struct Imp;

#ifdef _WIN32

// Windows reactor — uses CreateThreadpoolTimer for one-shot timers
// and WSAEventSelect + RegisterWaitForSingleObject for fd events.
// No dedicated reactor thread; the OS thread pool dispatches callbacks.

class Reactor {
public:
    static Reactor& instance();

    std::pair<reader<>, uintptr_t> create_timer(int64_t delay_ns);
    void cancel_timer(uintptr_t ident);

    // Create a one-shot fd readiness event. kind: 0=read, 1=write.
    // Returns (reader, ident) — caller wraps in fd_signal for RAII.
    std::pair<reader<>, uintptr_t> create_fd_event(SOCKET sock, int kind);

    // Cancel an fd event by ident. No-op if already fired.
    void cancel_fd(uintptr_t ident);

    void ensure_started();

    bool has_pending_signals() const {
        return pending_signals_.load(std::memory_order_acquire) > 0;
    }

    void shutdown();

private:
    Reactor() = default;
    Reactor(Reactor const&) = delete;
    Reactor& operator=(Reactor const&) = delete;

    static VOID CALLBACK timer_callback(
        PTP_CALLBACK_INSTANCE instance,
        PVOID context,
        PTP_TIMER timer);

    static VOID CALLBACK fd_callback(
        PVOID context,
        BOOLEAN timed_out);

    struct TimerEntry {
        writer<> w;
        PTP_TIMER handle;
    };

    struct FdEntry {
        writer<> w;
        HANDLE event;       // WSACreateEvent
        HANDLE wait_handle; // RegisterWaitForSingleObject
        SOCKET sock;        // for WSAEventSelect cleanup
    };

    std::mutex start_mu_;
    std::atomic<bool> running_{false};

    // Protects timer_entries_ and fd_entries_. Lock ordering:
    // signal_mu_ before any channel lock and before global_mu / run_mu.
    std::mutex signal_mu_;

    std::atomic<uintptr_t> next_ident_{1};
    std::atomic<int> pending_signals_{0};

    std::unordered_map<uintptr_t, TimerEntry> timer_entries_;
    std::unordered_map<uintptr_t, FdEntry> fd_entries_;
};

#else // !_WIN32

class Reactor {
public:
    static Reactor& instance();

    // --- Signal-based API ---
    // These create platform events whose firing drops a writer<>,
    // producing a death signal observable via prialt(~reader).

    // Create a one-shot timer. Returns (reader, ident).
    // Caller wraps in timer_signal for RAII cancellation.
    std::pair<reader<>, uintptr_t> create_timer(int64_t delay_ns);

    // Create a one-shot fd readiness event. Returns reader.
    // Caller wraps in fd_signal for RAII cancellation.
    reader<> create_fd_event(int fd, fd_event event);

    // Cancel a timer by ident. No-op if already fired.
    void cancel_timer(uintptr_t ident);

    // Cancel an fd event. No-op if already fired.
    void cancel_fd(int fd, fd_event event);

    // Lazy init: creates event fd and spawns reactor thread on first call.
    void ensure_started();

    // True if there are pending reactor signals (timers or fd events)
    // that haven't fired yet.
    bool has_pending_signals() const {
        return pending_signals_.load(std::memory_order_acquire) > 0;
    }

    // Stop reactor thread and close fds. Idempotent.
    void shutdown();

private:
    Reactor() = default;
    Reactor(Reactor const&) = delete;
    Reactor& operator=(Reactor const&) = delete;

    void loop();
    void wake();
    void fire_signal(uintptr_t ident, fd_event event);

#ifdef __APPLE__
    int kq_ = -1;
#elif defined(__linux__)
    int epfd_ = -1;
    int wakefd_ = -1;
    std::unordered_map<int, uintptr_t> timerfd_to_ident_;
    std::unordered_map<uintptr_t, int> ident_to_timerfd_;
#endif

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::mutex start_mu_;

    // Protects writer maps (and timerfd maps on Linux).
    // Lock ordering: signal_mu_ before any channel lock (channel_mu_)
    // and before global_mu / run_mu.
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
