#include <csp/internal/reactor.h>
#include <csp/internal/signal.h>

#include <cassert>

#ifdef _WIN32

// --- Windows reactor: CreateThreadpoolTimer-based ---

namespace csp::detail {

Reactor& Reactor::instance() {
    static Reactor r;
    return r;
}

void Reactor::ensure_started() {
    if (running_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(start_mu_);
    if (running_.load(std::memory_order_relaxed)) return;
    running_.store(true, std::memory_order_release);
}

void Reactor::shutdown() {
    if (!running_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(start_mu_);
    if (!running_.load(std::memory_order_relaxed)) return;

    // Collect handles under lock, clear map (writer dtors fire death signals).
    std::vector<PTP_TIMER> handles;
    {
        std::lock_guard<std::mutex> slk(signal_mu_);
        handles.reserve(timer_entries_.size());
        for (auto& [ident, entry] : timer_entries_)
            handles.push_back(entry.handle);
        timer_entries_.clear();
    }
    pending_signals_.store(0, std::memory_order_release);

    // Outside lock: disarm and close each handle.
    for (auto h : handles) {
        SetThreadpoolTimer(h, NULL, 0, 0);
        WaitForThreadpoolTimerCallbacks(h, TRUE);
        CloseThreadpoolTimer(h);
    }

    running_.store(false, std::memory_order_release);
}

std::pair<reader<>, uintptr_t> Reactor::create_timer(int64_t delay_ns) {
    chan<> ch;
    auto ident = next_ident_.fetch_add(1, std::memory_order_relaxed);

    PTP_TIMER tp_timer = CreateThreadpoolTimer(
        timer_callback,
        reinterpret_cast<PVOID>(static_cast<uintptr_t>(ident)),
        NULL);
    assert(tp_timer);

    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        timer_entries_.emplace(ident,
                               TimerEntry{std::move(ch.w), tp_timer});
    }
    pending_signals_.fetch_add(1, std::memory_order_release);

    // Convert nanoseconds to negative FILETIME ticks (100ns units, relative).
    LARGE_INTEGER li;
    li.QuadPart = -(static_cast<LONGLONG>(delay_ns) / 100);
    if (li.QuadPart == 0 && delay_ns > 0)
        li.QuadPart = -1;  // minimum 100ns
    FILETIME due_time;
    due_time.dwLowDateTime = li.LowPart;
    due_time.dwHighDateTime = li.HighPart;

    SetThreadpoolTimer(tp_timer, &due_time, 0, 0);  // one-shot

    return {std::move(ch.r), ident};
}

void Reactor::cancel_timer(uintptr_t ident) {
    PTP_TIMER handle = nullptr;
    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        auto it = timer_entries_.find(ident);
        if (it == timer_entries_.end()) return;  // already fired
        handle = it->second.handle;
        timer_entries_.erase(it);  // writer dtor fires death signal
        pending_signals_.fetch_sub(1, std::memory_order_release);
    }
    // Outside lock: safe to wait for callback completion.
    SetThreadpoolTimer(handle, NULL, 0, 0);
    WaitForThreadpoolTimerCallbacks(handle, TRUE);
    CloseThreadpoolTimer(handle);
}

VOID CALLBACK Reactor::timer_callback(
    PTP_CALLBACK_INSTANCE /*instance*/,
    PVOID context,
    PTP_TIMER timer)
{
    auto ident = reinterpret_cast<uintptr_t>(context);
    auto& reactor = Reactor::instance();

    bool erased = false;
    {
        std::lock_guard<std::mutex> lk(reactor.signal_mu_);
        auto it = reactor.timer_entries_.find(ident);
        if (it != reactor.timer_entries_.end()) {
            reactor.timer_entries_.erase(it);  // writer dtor fires death signal
            erased = true;
        }
    }
    if (erased) {
        reactor.pending_signals_.fetch_sub(1, std::memory_order_release);
        CloseThreadpoolTimer(timer);
    }
    // If !erased, cancel_timer already handled everything.
}

} // namespace csp::detail

#elif defined(__APPLE__)

// --- macOS reactor: kqueue-based ---

#include <sys/event.h>
#include <unistd.h>

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

} // namespace csp::detail

#endif // platform selection

// --- timer_signal (all platforms) ---

namespace csp::detail {

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

// --- Factory ---

timer_signal create_timer_signal(int64_t delay_ns) {
    auto& reactor = Reactor::instance();
    reactor.ensure_started();
    auto [r, ident] = reactor.create_timer(delay_ns);
    return {std::move(r), ident};
}

} // namespace csp::detail

// --- fd_signal (Unix only) ---

#ifndef _WIN32

#include <sys/event.h>

namespace csp::detail {

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

#endif // !_WIN32
