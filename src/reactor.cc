#include <csp/internal/reactor.h>
#include <csp/internal/csp_internal.h>

#include <sys/event.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>

namespace csp::detail {

Reactor& Reactor::instance() {
    static Reactor r;
    return r;
}

void Reactor::ensure_started() {
    if (running_.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lk(start_mu_);
    if (running_.load(std::memory_order_relaxed)) return;

    stopping_.store(false, std::memory_order_relaxed);

    kq_ = kqueue();
    assert(kq_ >= 0);

    // Register EVFILT_USER (ident=0) for shutdown wakeup.
    struct kevent ev;
    EV_SET(&ev, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    int rc = kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    assert(rc == 0);

    thread_ = std::thread([this] { loop(); });
    running_.store(true, std::memory_order_release);
}

void Reactor::shutdown() {
    if (!running_.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lk(start_mu_);
    if (!running_.load(std::memory_order_relaxed)) return;

    stopping_.store(true, std::memory_order_release);
    wake();
    thread_.join();

    close(kq_);
    kq_ = -1;
    running_.store(false, std::memory_order_release);
}

void Reactor::wake() {
    struct kevent ev;
    EV_SET(&ev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    kevent(kq_, &ev, 1, nullptr, 0, nullptr);
}

void Reactor::wait_read(int fd, Microthread* mt) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, mt);
    int rc = kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    assert(rc == 0);
}

void Reactor::wait_write(int fd, Microthread* mt) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, mt);
    int rc = kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    assert(rc == 0);
}

void Reactor::cancel(int fd) {
    struct kevent evs[2];
    EV_SET(&evs[0], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&evs[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    // Ignore errors — fd may not be registered for both filters.
    kevent(kq_, evs, 2, nullptr, 0, nullptr);
}

void Reactor::loop() {
    struct kevent events[64];
    while (!stopping_.load(std::memory_order_acquire)) {
        int n = kevent(kq_, nullptr, 0, events, 64, nullptr);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (events[i].filter == EVFILT_USER) continue;
            auto* mt = static_cast<Microthread*>(events[i].udata);
            mt->schedule();
        }
    }
}

} // namespace csp::detail

// Layer 1 primitives — follow the sleep_until suspension pattern
// (csp.cc:399-406). Critical: set suspending_=true BEFORE registering
// with the reactor, because the reactor (another thread) can call
// schedule() immediately. This mirrors the channel path where
// suspending_ is set before unlock_all.

namespace csp::internal {

void io_wait_readable(int fd) {
    auto& reactor = detail::Reactor::instance();
    reactor.ensure_started();

    detail::g_self->suspending_.store(true, std::memory_order_release);
    reactor.wait_read(fd, detail::g_self);
    detail::do_switch(detail::Status::detach);
    detail::g_self->suspending_.store(false, std::memory_order_release);
}

void io_wait_writable(int fd) {
    auto& reactor = detail::Reactor::instance();
    reactor.ensure_started();

    detail::g_self->suspending_.store(true, std::memory_order_release);
    reactor.wait_write(fd, detail::g_self);
    detail::do_switch(detail::Status::detach);
    detail::g_self->suspending_.store(false, std::memory_order_release);
}

} // namespace csp::internal
