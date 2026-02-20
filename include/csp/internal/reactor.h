#pragma once

#include <atomic>
#include <mutex>
#include <thread>

namespace csp::detail {

struct Imp;

class Reactor {
public:
    static Reactor& instance();

    // Register fd for read/write readiness. The imp will be
    // rescheduled via imp->schedule() when the event fires. EV_ONESHOT
    // semantics: each registration fires at most once.
    void wait_read(int fd, Imp* imp);
    void wait_write(int fd, Imp* imp);

    // Remove all registrations for fd.
    void cancel(int fd);

    // Lazy init: creates kqueue fd and spawns reactor thread on first call.
    void ensure_started();

    // Stop reactor thread and close kqueue fd. Idempotent.
    void shutdown();

private:
    Reactor() = default;
    Reactor(Reactor const&) = delete;
    Reactor& operator=(Reactor const&) = delete;

    void loop();
    void wake();

    int kq_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::mutex start_mu_;
};

}
