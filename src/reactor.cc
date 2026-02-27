#include <csp/internal/reactor.h>
#include <csp/internal/signal.h>

#ifdef _WIN32

// --- Windows stub reactor (Phase 2 will implement) ---

#include <stdexcept>

namespace csp::detail {

Reactor& Reactor::instance() {
    static Reactor r;
    return r;
}

void Reactor::ensure_started() {}
void Reactor::shutdown() {}

std::pair<reader<>, uintptr_t> Reactor::create_timer(int64_t) {
    throw std::runtime_error("csp: reactor not yet available on Windows");
}

void Reactor::cancel_timer(uintptr_t) {}

} // namespace csp::detail

#else // !_WIN32

#include <sys/event.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>

namespace csp::detail {

// --- Reactor singleton ---

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

    // Clear remaining writers (events that never fired).
    {
        std::lock_guard<std::mutex> slk(signal_mu_);
        timer_writers_.clear();
        read_writers_.clear();
        write_writers_.clear();
    }
    pending_signals_.store(0, std::memory_order_release);

    running_.store(false, std::memory_order_release);
}

void Reactor::wake() {
    struct kevent ev;
    EV_SET(&ev, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    kevent(kq_, &ev, 1, nullptr, 0, nullptr);
}

// --- Signal creation ---

std::pair<reader<>, uintptr_t> Reactor::create_timer(int64_t delay_ns) {
    chan<> ch;
    auto ident = next_ident_.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        timer_writers_.emplace(ident, std::move(ch.w));
    }
    pending_signals_.fetch_add(1, std::memory_order_release);

    struct kevent ev;
    EV_SET(&ev, ident, EVFILT_TIMER, EV_ADD | EV_ONESHOT,
           NOTE_NSECONDS, delay_ns, nullptr);
    int rc = kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    assert(rc == 0);

    return {std::move(ch.r), ident};
}

reader<> Reactor::create_fd_event(int fd, int16_t filter) {
    chan<> ch;

    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        if (filter == EVFILT_READ)
            read_writers_.insert_or_assign(fd, std::move(ch.w));
        else
            write_writers_.insert_or_assign(fd, std::move(ch.w));
    }
    pending_signals_.fetch_add(1, std::memory_order_release);

    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_ADD | EV_ONESHOT, 0, 0, nullptr);
    int rc = kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    assert(rc == 0);

    return std::move(ch.r);
}

// --- Signal cancellation ---

void Reactor::cancel_timer(uintptr_t ident) {
    // EV_DELETE first (while kq_ is still valid), then erase writer.
    struct kevent ev;
    EV_SET(&ev, ident, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
    kevent(kq_, &ev, 1, nullptr, 0, nullptr);  // ignore error (may have fired)

    std::lock_guard<std::mutex> lk(signal_mu_);
    if (timer_writers_.erase(ident))
        pending_signals_.fetch_sub(1, std::memory_order_release);
}

void Reactor::cancel_fd(int fd, int16_t filter) {
    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_DELETE, 0, 0, nullptr);
    kevent(kq_, &ev, 1, nullptr, 0, nullptr);  // ignore error

    std::lock_guard<std::mutex> lk(signal_mu_);
    size_t erased = (filter == EVFILT_READ)
        ? read_writers_.erase(fd)
        : write_writers_.erase(fd);
    if (erased)
        pending_signals_.fetch_sub(1, std::memory_order_release);
}

// --- Reactor loop ---

void Reactor::fire_signal(uintptr_t ident, int16_t filter) {
    size_t erased = 0;
    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        switch (filter) {
        case EVFILT_TIMER: erased = timer_writers_.erase(ident); break;
        case EVFILT_READ:  erased = read_writers_.erase(static_cast<int>(ident)); break;
        case EVFILT_WRITE: erased = write_writers_.erase(static_cast<int>(ident)); break;
        }
        // writer<> destructor runs here -> writer_release -> resolve_endpoint_death
        // -> wakes any imp in prialt watching ~reader on the same channel.
    }
    if (erased)
        pending_signals_.fetch_sub(1, std::memory_order_release);
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
            fire_signal(events[i].ident, events[i].filter);
        }
    }
}

// --- timer_signal ---

timer_signal::timer_signal(reader<> r, uintptr_t ident)
    : r_(std::move(r)), ident_(ident) {}

timer_signal::timer_signal(timer_signal&& o) noexcept
    : r_(std::move(o.r_)), ident_(o.ident_) {
    o.ident_ = 0;
}

timer_signal& timer_signal::operator=(timer_signal&& o) noexcept {
    if (this != &o) {
        if (ident_) Reactor::instance().cancel_timer(ident_);
        r_ = std::move(o.r_);
        ident_ = o.ident_;
        o.ident_ = 0;
    }
    return *this;
}

timer_signal::~timer_signal() {
    if (ident_) Reactor::instance().cancel_timer(ident_);
}

// --- fd_signal ---

fd_signal::fd_signal(reader<> r, int fd, int16_t filter)
    : r_(std::move(r)), fd_(fd), filter_(filter) {}

fd_signal::fd_signal(fd_signal&& o) noexcept
    : r_(std::move(o.r_)), fd_(o.fd_), filter_(o.filter_) {
    o.fd_ = -1;
    o.filter_ = 0;
}

fd_signal& fd_signal::operator=(fd_signal&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) Reactor::instance().cancel_fd(fd_, filter_);
        r_ = std::move(o.r_);
        fd_ = o.fd_;
        filter_ = o.filter_;
        o.fd_ = -1;
        o.filter_ = 0;
    }
    return *this;
}

fd_signal::~fd_signal() {
    if (fd_ >= 0) Reactor::instance().cancel_fd(fd_, filter_);
}

// --- Factory functions ---

timer_signal create_timer_signal(int64_t delay_ns) {
    auto& reactor = Reactor::instance();
    reactor.ensure_started();
    auto [r, ident] = reactor.create_timer(delay_ns);
    return {std::move(r), ident};
}

fd_signal create_fd_readable(int fd) {
    auto& reactor = Reactor::instance();
    reactor.ensure_started();
    auto r = reactor.create_fd_event(fd, EVFILT_READ);
    return {std::move(r), fd, EVFILT_READ};
}

fd_signal create_fd_writable(int fd) {
    auto& reactor = Reactor::instance();
    reactor.ensure_started();
    auto r = reactor.create_fd_event(fd, EVFILT_WRITE);
    return {std::move(r), fd, EVFILT_WRITE};
}

} // namespace csp::detail

#endif // _WIN32
