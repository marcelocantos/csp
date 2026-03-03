#include <csp/internal/reactor.h>
#include <csp/internal/signal.h>

#ifdef __APPLE__
#include <sys/event.h>
#elif defined(__linux__)
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#endif
#include <unistd.h>

#include <cassert>
#include <cerrno>

namespace csp::detail {

// --- Reactor singleton ---

Reactor& Reactor::instance() {
    static Reactor r;
    return r;
}

// ============================================================
// Platform: macOS (kqueue)
// ============================================================

#ifdef __APPLE__

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

reader<> Reactor::create_fd_event(int fd, fd_event event) {
    chan<> ch;

    int16_t filter = (event == fd_event::read) ? EVFILT_READ : EVFILT_WRITE;

    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        if (event == fd_event::read)
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

void Reactor::cancel_timer(uintptr_t ident) {
    struct kevent ev;
    EV_SET(&ev, ident, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
    kevent(kq_, &ev, 1, nullptr, 0, nullptr);  // ignore error (may have fired)

    std::lock_guard<std::mutex> lk(signal_mu_);
    if (timer_writers_.erase(ident))
        pending_signals_.fetch_sub(1, std::memory_order_release);
}

void Reactor::cancel_fd(int fd, fd_event event) {
    int16_t filter = (event == fd_event::read) ? EVFILT_READ : EVFILT_WRITE;

    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_DELETE, 0, 0, nullptr);
    kevent(kq_, &ev, 1, nullptr, 0, nullptr);  // ignore error

    std::lock_guard<std::mutex> lk(signal_mu_);
    size_t erased = (event == fd_event::read)
        ? read_writers_.erase(fd)
        : write_writers_.erase(fd);
    if (erased)
        pending_signals_.fetch_sub(1, std::memory_order_release);
}

void Reactor::fire_signal(uintptr_t ident, fd_event event) {
    size_t erased = 0;
    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        // On macOS, timer fire_signal is called with fd_event::read
        // as a sentinel — we dispatch on ident presence in timer_writers_.
        erased = timer_writers_.erase(ident);
        if (!erased) {
            int fd = static_cast<int>(ident);
            if (event == fd_event::read)
                erased = read_writers_.erase(fd);
            else
                erased = write_writers_.erase(fd);
        }
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
            fd_event ev = (events[i].filter == EVFILT_WRITE)
                ? fd_event::write : fd_event::read;
            fire_signal(events[i].ident, ev);
        }
    }
}

// ============================================================
// Platform: Linux (epoll + timerfd + eventfd)
// ============================================================

#elif defined(__linux__)

void Reactor::ensure_started() {
    if (running_.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lk(start_mu_);
    if (running_.load(std::memory_order_relaxed)) return;

    stopping_.store(false, std::memory_order_relaxed);

    epfd_ = epoll_create1(0);
    assert(epfd_ >= 0);

    // Create eventfd for shutdown/wakeup.
    wakefd_ = eventfd(0, EFD_NONBLOCK);
    assert(wakefd_ >= 0);

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wakefd_;
    int rc = epoll_ctl(epfd_, EPOLL_CTL_ADD, wakefd_, &ev);
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

    // Close all timerfd descriptors.
    {
        std::lock_guard<std::mutex> slk(signal_mu_);
        for (auto& [tfd, ident] : timerfd_to_ident_)
            close(tfd);
        timerfd_to_ident_.clear();
        ident_to_timerfd_.clear();
        timer_writers_.clear();
        read_writers_.clear();
        write_writers_.clear();
    }

    close(wakefd_);
    wakefd_ = -1;
    close(epfd_);
    epfd_ = -1;

    pending_signals_.store(0, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

void Reactor::wake() {
    uint64_t val = 1;
    [[maybe_unused]] auto n = ::write(wakefd_, &val, sizeof(val));
}

std::pair<reader<>, uintptr_t> Reactor::create_timer(int64_t delay_ns) {
    chan<> ch;
    auto ident = next_ident_.fetch_add(1, std::memory_order_relaxed);

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    assert(tfd >= 0);

    struct itimerspec ts{};
    ts.it_value.tv_sec  = delay_ns / 1'000'000'000;
    ts.it_value.tv_nsec = delay_ns % 1'000'000'000;
    // Zero delay: arm with 1 ns to ensure the timer fires.
    if (ts.it_value.tv_sec == 0 && ts.it_value.tv_nsec == 0)
        ts.it_value.tv_nsec = 1;
    int rc = timerfd_settime(tfd, 0, &ts, nullptr);
    assert(rc == 0);

    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        timer_writers_.emplace(ident, std::move(ch.w));
        timerfd_to_ident_.emplace(tfd, ident);
        ident_to_timerfd_.emplace(ident, tfd);
    }
    pending_signals_.fetch_add(1, std::memory_order_release);

    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.fd = tfd;
    rc = epoll_ctl(epfd_, EPOLL_CTL_ADD, tfd, &ev);
    assert(rc == 0);

    return {std::move(ch.r), ident};
}

reader<> Reactor::create_fd_event(int fd, fd_event event) {
    chan<> ch;

    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        if (event == fd_event::read)
            read_writers_.insert_or_assign(fd, std::move(ch.w));
        else
            write_writers_.insert_or_assign(fd, std::move(ch.w));
    }
    pending_signals_.fetch_add(1, std::memory_order_release);

    struct epoll_event ev{};
    ev.events = ((event == fd_event::read) ? EPOLLIN : EPOLLOUT)
              | EPOLLONESHOT;
    ev.data.fd = fd;
    int rc = epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev);
    assert(rc == 0);

    return std::move(ch.r);
}

void Reactor::cancel_timer(uintptr_t ident) {
    std::lock_guard<std::mutex> lk(signal_mu_);
    auto it = ident_to_timerfd_.find(ident);
    if (it != ident_to_timerfd_.end()) {
        int tfd = it->second;
        epoll_ctl(epfd_, EPOLL_CTL_DEL, tfd, nullptr);
        close(tfd);
        timerfd_to_ident_.erase(tfd);
        ident_to_timerfd_.erase(it);
    }
    if (timer_writers_.erase(ident))
        pending_signals_.fetch_sub(1, std::memory_order_release);
}

void Reactor::cancel_fd(int fd, fd_event event) {
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);  // ignore error

    std::lock_guard<std::mutex> lk(signal_mu_);
    size_t erased = (event == fd_event::read)
        ? read_writers_.erase(fd)
        : write_writers_.erase(fd);
    if (erased)
        pending_signals_.fetch_sub(1, std::memory_order_release);
}

void Reactor::fire_signal(uintptr_t ident, fd_event event) {
    size_t erased = 0;
    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        erased = timer_writers_.erase(ident);
        if (!erased) {
            int fd = static_cast<int>(ident);
            if (event == fd_event::read)
                erased = read_writers_.erase(fd);
            else
                erased = write_writers_.erase(fd);
        }
    }
    if (erased)
        pending_signals_.fetch_sub(1, std::memory_order_release);
}

void Reactor::loop() {
    struct epoll_event events[64];
    while (!stopping_.load(std::memory_order_acquire)) {
        int n = epoll_wait(epfd_, events, 64, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            // Wakeup event — drain and continue.
            if (fd == wakefd_) {
                uint64_t val;
                [[maybe_unused]] auto nr = ::read(wakefd_, &val, sizeof(val));
                continue;
            }

            // Check if this is a timerfd.
            uintptr_t ident;
            fd_event ev;
            {
                std::lock_guard<std::mutex> lk(signal_mu_);
                auto it = timerfd_to_ident_.find(fd);
                if (it != timerfd_to_ident_.end()) {
                    ident = it->second;
                    // Drain the timerfd.
                    uint64_t val;
                    [[maybe_unused]] auto nr = ::read(fd, &val, sizeof(val));
                    // Clean up timerfd.
                    close(fd);
                    timerfd_to_ident_.erase(it);
                    ident_to_timerfd_.erase(ident);
                    ev = fd_event::read;  // sentinel for timer
                } else {
                    ident = static_cast<uintptr_t>(fd);
                    ev = (events[i].events & EPOLLOUT)
                        ? fd_event::write : fd_event::read;
                }
            }
            fire_signal(ident, ev);
        }
    }
}

#endif // __APPLE__ / __linux__

// ============================================================
// Shared: timer_signal / fd_signal RAII + factory functions
// ============================================================

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

fd_signal::fd_signal(reader<> r, int fd, fd_event event)
    : r_(std::move(r)), fd_(fd), event_(event) {}

fd_signal::fd_signal(fd_signal&& o) noexcept
    : r_(std::move(o.r_)), fd_(o.fd_), event_(o.event_) {
    o.fd_ = -1;
}

fd_signal& fd_signal::operator=(fd_signal&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) Reactor::instance().cancel_fd(fd_, event_);
        r_ = std::move(o.r_);
        fd_ = o.fd_;
        event_ = o.event_;
        o.fd_ = -1;
    }
    return *this;
}

fd_signal::~fd_signal() {
    if (fd_ >= 0) Reactor::instance().cancel_fd(fd_, event_);
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
    auto r = reactor.create_fd_event(fd, fd_event::read);
    return {std::move(r), fd, fd_event::read};
}

fd_signal create_fd_writable(int fd) {
    auto& reactor = Reactor::instance();
    reactor.ensure_started();
    auto r = reactor.create_fd_event(fd, fd_event::write);
    return {std::move(r), fd, fd_event::write};
}

} // namespace csp::detail
