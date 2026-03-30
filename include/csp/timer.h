#pragma once

#include <csp/csp.h>
#include <csp/dynamic.h>

#include <chrono>
#include <memory>
#include <queue>
#include <vector>

namespace csp::detail { struct Imp; }

namespace csp {

using time_point = std::chrono::steady_clock::time_point;
using duration = std::chrono::steady_clock::duration;

// Abstract clock source. The dynamic variable csp::clock always holds a
// valid pointer — real_clock by default, fake_clock when bound via
// csp::local l{csp::clock = &fc}.
struct clock_source {
    virtual time_point now() const = 0;
    virtual void sleep_until(time_point tp) = 0;
    // True for the real clock (reactor-backed timers).
    // False for fake_clock (cooperative timer queue).
    virtual bool uses_reactor() const { return true; }
    virtual ~clock_source() = default;
};

// Fake clock for testing time-dependent code.
// Bind via csp::local l{csp::clock = &fc}.
class fake_clock : public clock_source {
    time_point current_;
    struct Entry {
        time_point deadline;
        detail::Imp* imp;
        bool operator>(Entry const& o) const { return deadline > o.deadline; }
    };
    std::mutex mu_;
    std::priority_queue<Entry, std::vector<Entry>,
                        std::greater<Entry>> pending_;
    quiescence_scope qs_;
    void fire_expired();

public:
    explicit fake_clock(time_point start = time_point{});

    time_point now() const override;
    void sleep_until(time_point tp) override;
    bool uses_reactor() const override { return false; }

    bool has_pending() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_));
        return !pending_.empty();
    }

    // Advance time and fire expired timers.
    void advance(duration d);

    // Jump to next pending deadline. Returns false if no timers pending.
    bool advance_to_next();

    // Bind the fake clock's quiescence scope to the current imp.
    // Call before spawning timer-using imps so they inherit the scope.
    void bind_scope() { qs_.bind(); }

    // Convenience: bind clock + quiescence scope in one call.
    // Returns a dynamic_binding for use with csp::local.
    // Usage: csp::local l{fc.binding()};
    //   — equivalent to: csp::local l{csp::clock = &fc}; fc.bind_scope();
    [[nodiscard]] dynamic_binding binding();

    // Run until all timer-blocked imps complete. If bind_scope()
    // was not called, binds automatically (but imps spawned before
    // this call won't be tracked). Loops: wait for quiescence →
    // advance time → repeat.
    void run();

    // Wait for quiescence (all scope members sleeping), then return.
    void run_until_idle();

    ~fake_clock();

    // Movable (for storage in shared_ptr via csp::clock = fake_clock{}).
    fake_clock(fake_clock&& o) noexcept : current_(o.current_) {}
    fake_clock& operator=(fake_clock&&) = delete;
    fake_clock(fake_clock const&) = delete;
    fake_clock& operator=(fake_clock const&) = delete;

    // Convert to shared_ptr for use with csp::clock dynamic variable.
    // Enables: csp::local l{csp::clock = fake_clock{}};
    operator std::shared_ptr<clock_source>() && {
        return std::make_shared<fake_clock>(std::move(*this));
    }
};

extern dynamic<std::shared_ptr<clock_source>> clock;

// Current time.
inline time_point now() {
    return (*clock)->now();
}

// Block the current imp until the given deadline.
// Cancel-aware: throws canceled/timed_out if a cancel scope fires.
void sleep_until(time_point tp);

// Block the current imp for the given duration.
// Cancel-aware: throws canceled/timed_out if a cancel scope fires.
void sleep(duration d);

// Return a reader that fires once after the given duration,
// delivering the current time. Uses a reactor timer signal;
// dropping the reader cancels the timer promptly.
reader<time_point> after(duration d);

// Return a reader that fires repeatedly at the given interval,
// delivering the current time. Uses absolute deadlines to prevent drift.
reader<time_point> tick(duration interval);

}
