#include <csp/internal/reactor.h>
#include <csp/internal/signal.h>

#include <cassert>

#ifdef _WIN32

// --- Windows reactor: CreateThreadpoolTimer + WSAEventSelect ---

#include <winsock2.h>

namespace csp::detail {

Reactor& Reactor::instance() {
    static Reactor r;
    return r;
}

void Reactor::ensure_started() {
    if (running_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(start_mu_);
    if (running_.load(std::memory_order_relaxed)) return;

    // Initialize Winsock (ref-counted, multiple calls are safe).
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    running_.store(true, std::memory_order_release);
}

void Reactor::shutdown() {
    if (!running_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(start_mu_);
    if (!running_.load(std::memory_order_relaxed)) return;

    // Collect timer handles under lock, clear map (writer dtors fire death signals).
    std::vector<PTP_TIMER> timer_handles;
    struct FdHandles { HANDLE event; HANDLE wait; SOCKET sock; };
    std::vector<FdHandles> fd_handles;
    {
        std::lock_guard<std::mutex> slk(signal_mu_);
        timer_handles.reserve(timer_entries_.size());
        for (auto& [ident, entry] : timer_entries_)
            timer_handles.push_back(entry.handle);
        timer_entries_.clear();

        fd_handles.reserve(fd_entries_.size());
        for (auto& [ident, entry] : fd_entries_)
            fd_handles.push_back({entry.event, entry.wait_handle, entry.sock});
        fd_entries_.clear();
    }
    pending_signals_.store(0, std::memory_order_release);

    // Outside lock: disarm and close timer handles.
    for (auto h : timer_handles) {
        SetThreadpoolTimer(h, NULL, 0, 0);
        WaitForThreadpoolTimerCallbacks(h, TRUE);
        CloseThreadpoolTimer(h);
    }

    // Outside lock: clean up fd event handles.
    for (auto& fh : fd_handles) {
        WSAEventSelect(fh.sock, NULL, 0);
        UnregisterWaitEx(fh.wait, INVALID_HANDLE_VALUE);
        WSACloseEvent(fh.event);
    }

    WSACleanup();
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

// --- fd events (WSAEventSelect + RegisterWaitForSingleObject) ---

std::pair<reader<>, uintptr_t> Reactor::create_fd_event(SOCKET sock, int kind) {
    chan<> ch;
    auto ident = next_ident_.fetch_add(1, std::memory_order_relaxed);

    HANDLE evt = WSACreateEvent();
    assert(evt != WSA_INVALID_EVENT);

    long net_events = (kind == 0) ? (FD_READ | FD_CLOSE) : (FD_WRITE | FD_CLOSE);
    WSAEventSelect(sock, evt, net_events);

    HANDLE wait = nullptr;
    [[maybe_unused]] BOOL ok = RegisterWaitForSingleObject(
        &wait, evt,
        fd_callback,
        reinterpret_cast<PVOID>(ident),
        INFINITE,
        WT_EXECUTEONLYONCE);
    assert(ok);

    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        fd_entries_.emplace(ident,
                            FdEntry{std::move(ch.w), evt, wait, sock});
    }
    pending_signals_.fetch_add(1, std::memory_order_release);

    return {std::move(ch.r), ident};
}

VOID CALLBACK Reactor::fd_callback(PVOID context, BOOLEAN /*timed_out*/) {
    auto ident = reinterpret_cast<uintptr_t>(context);
    auto& reactor = Reactor::instance();

    HANDLE evt = nullptr;
    HANDLE wait = nullptr;
    SOCKET sock = INVALID_SOCKET;
    bool erased = false;
    {
        std::lock_guard<std::mutex> lk(reactor.signal_mu_);
        auto it = reactor.fd_entries_.find(ident);
        if (it != reactor.fd_entries_.end()) {
            evt = it->second.event;
            wait = it->second.wait_handle;
            sock = it->second.sock;
            reactor.fd_entries_.erase(it);  // writer dtor fires death signal
            erased = true;
        }
    }
    if (erased) {
        reactor.pending_signals_.fetch_sub(1, std::memory_order_release);
        WSAEventSelect(sock, NULL, 0);
        // Non-blocking unregister — we are inside the callback.
        UnregisterWaitEx(wait, NULL);
        WSACloseEvent(evt);
    }
    // If !erased, cancel_fd already handled everything.
}

void Reactor::cancel_fd(uintptr_t ident) {
    HANDLE evt = nullptr;
    HANDLE wait = nullptr;
    SOCKET sock = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lk(signal_mu_);
        auto it = fd_entries_.find(ident);
        if (it == fd_entries_.end()) return;  // already fired
        evt = it->second.event;
        wait = it->second.wait_handle;
        sock = it->second.sock;
        fd_entries_.erase(it);  // writer dtor fires death signal
    }
    pending_signals_.fetch_sub(1, std::memory_order_release);
    WSAEventSelect(sock, NULL, 0);
    // Blocking wait for callback completion (safe — called from outside callback).
    UnregisterWaitEx(wait, INVALID_HANDLE_VALUE);
    WSACloseEvent(evt);
}

} // namespace csp::detail

#else // !_WIN32

// --- Unix reactor: macOS (kqueue) / Linux (epoll) ---

#ifdef __APPLE__
#include <sys/event.h>
#elif defined(__linux__)
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#endif
#include <unistd.h>

#include <cerrno>

namespace csp::detail {

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
    if (rc < 0 && errno == EEXIST) {
        // After EPOLLONESHOT fires, epoll keeps the fd registered but disabled.
        // Re-arm it with MOD instead of ADD (epoll(7): EPOLL_CTL_ADD returns
        // EEXIST for an already-registered fd, even one that has been one-shot
        // disarmed).  kqueue EV_ADD | EV_ONESHOT is idempotent; epoll is not.
        rc = epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
    }
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

} // namespace csp::detail

#endif // _WIN32

// ============================================================
// Shared: timer_signal / fd_signal RAII + factory functions
// ============================================================

namespace csp::detail {

// --- timer_signal (all platforms) ---

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

timer_signal create_timer_signal(int64_t delay_ns) {
    auto& reactor = Reactor::instance();
    reactor.ensure_started();
    auto [r, ident] = reactor.create_timer(delay_ns);
    return {std::move(r), ident};
}

// --- fd_signal ---

#ifdef _WIN32

fd_signal::fd_signal(reader<> r, uintptr_t ident)
    : r_(std::move(r)), ident_(ident) {}

fd_signal::fd_signal(fd_signal&& o) noexcept
    : r_(std::move(o.r_)), ident_(o.ident_) {
    o.ident_ = 0;
}

fd_signal& fd_signal::operator=(fd_signal&& o) noexcept {
    if (this != &o) {
        if (ident_) Reactor::instance().cancel_fd(ident_);
        r_ = std::move(o.r_);
        ident_ = o.ident_;
        o.ident_ = 0;
    }
    return *this;
}

fd_signal::~fd_signal() {
    if (ident_) Reactor::instance().cancel_fd(ident_);
}

fd_signal create_fd_readable(SOCKET sock) {
    auto& reactor = Reactor::instance();
    reactor.ensure_started();
    auto [r, ident] = reactor.create_fd_event(sock, 0);
    return {std::move(r), ident};
}

fd_signal create_fd_writable(SOCKET sock) {
    auto& reactor = Reactor::instance();
    reactor.ensure_started();
    auto [r, ident] = reactor.create_fd_event(sock, 1);
    return {std::move(r), ident};
}

#else // !_WIN32

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

#endif // _WIN32

} // namespace csp::detail
