#pragma once

#include <csp/csp.h>

#include <cstdint>
#ifdef _WIN32
#include <winsock2.h>
#endif

namespace csp::detail {

// Platform-neutral fd readiness event type.
enum class fd_event : int8_t { read, write };

// RAII wrapper for a reactor timer event.
// Holds a reader whose peer writer is owned by the reactor.
// When the timer fires, the reactor drops the writer -> death signal.
// When this object is destroyed, the reactor event is cancelled and
// the reactor's writer is erased (triggering death if still alive).
class timer_signal {
    reader<> r_;
    uintptr_t ident_ = 0;

public:
    timer_signal() = default;
    timer_signal(reader<> r, uintptr_t ident);
    timer_signal(timer_signal&&) noexcept;
    timer_signal& operator=(timer_signal&&) noexcept;
    ~timer_signal();

    chan_op<> operator~() const { return ~r_; }
};

// Factory function — create a reactor timer and return the signal object.
// The reactor must be started before calling this.
timer_signal create_timer_signal(int64_t delay_ns);

#ifdef _WIN32

// RAII wrapper for a reactor fd-readiness event (Windows).
// Uses ident-based cancellation (same pattern as timer_signal).
class fd_signal {
    reader<> r_;
    uintptr_t ident_ = 0;

public:
    fd_signal() = default;
    fd_signal(reader<> r, uintptr_t ident);
    fd_signal(fd_signal&&) noexcept;
    fd_signal& operator=(fd_signal&&) noexcept;
    ~fd_signal();

    chan_op<> operator~() const { return ~r_; }
};

// Factory functions — create a WSAEventSelect event and return the signal.
fd_signal create_fd_readable(SOCKET sock);
fd_signal create_fd_writable(SOCKET sock);

#else // !_WIN32

// RAII wrapper for a reactor fd-readiness event.
// Same death-signal pattern as timer_signal.
class fd_signal {
    reader<> r_;
    int fd_ = -1;
    fd_event event_{};

public:
    fd_signal() = default;
    fd_signal(reader<> r, int fd, fd_event event);
    fd_signal(fd_signal&&) noexcept;
    fd_signal& operator=(fd_signal&&) noexcept;
    ~fd_signal();

    chan_op<> operator~() const { return ~r_; }
};

// Factory functions — create a kqueue/epoll event and return the signal object.
fd_signal create_fd_readable(int fd);
fd_signal create_fd_writable(int fd);

#endif // _WIN32

}
