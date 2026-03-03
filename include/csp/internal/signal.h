#pragma once

#include <csp/csp.h>

#include <cstdint>

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

// Factory functions — create a reactor event and return the signal object.
// The reactor must be started before calling these.
timer_signal create_timer_signal(int64_t delay_ns);
fd_signal create_fd_readable(int fd);
fd_signal create_fd_writable(int fd);

}
