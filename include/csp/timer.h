#pragma once

#include <csp/csp.h>
#include <csp/dynamic.h>

#include <chrono>
#include <queue>
#include <vector>

namespace csp::detail { struct Imp; }

namespace csp {

using clock = std::chrono::steady_clock;

// Fake clock for testing time-dependent code.
// Bind via csp::local l{csp::clock_override = &fc}.
class fake_clock {
    clock::time_point current_;
    struct Entry {
        clock::time_point deadline;
        detail::Imp* imp;
        bool operator>(Entry const& o) const { return deadline > o.deadline; }
    };
    std::priority_queue<Entry, std::vector<Entry>,
                        std::greater<Entry>> pending_;
    void fire_expired();

public:
    explicit fake_clock(clock::time_point start = clock::time_point{});

    clock::time_point now() const { return current_; }
    bool has_pending() const { return !pending_.empty(); }

    // Advance time and fire expired timers.
    void advance(clock::duration d);

    // Jump to next pending deadline. Returns false if no timers pending.
    bool advance_to_next();

    // Run scheduler loop with auto-advance until no work remains.
    void run();

    // Run scheduler until no imps are runnable (don't advance time).
    void run_until_idle();

    // Called by sleep_until when this clock is active.
    void sleep_until_impl(clock::time_point tp);

    fake_clock(fake_clock const&) = delete;
    fake_clock& operator=(fake_clock const&) = delete;
};

extern dynamic<fake_clock*> clock_override;

// Current time: fake if clock_override is bound, real otherwise.
inline clock::time_point now() {
    if (auto* fc = *clock_override)
        return fc->now();
    return clock::now();
}

// Block the current imp until the given deadline.
inline void sleep_until(clock::time_point tp) {
    if (auto* fc = *clock_override)
        return fc->sleep_until_impl(tp);
    internal::sleep_until(tp.time_since_epoch().count());
}

// Block the current imp for the given duration.
inline void sleep(clock::duration d) {
    sleep_until(csp::now() + d);
}

// Return a reader that fires once after the given duration,
// delivering the current time.
inline reader<clock::time_point> after(clock::duration d) {
    return spawn_producer<clock::time_point>([d](writer<clock::time_point> w) {
        csp::sleep(d);
        w << csp::now();
    });
}

// Return a reader that fires repeatedly at the given interval,
// delivering the current time. Uses absolute deadlines to prevent drift.
inline reader<clock::time_point> tick(clock::duration interval) {
    return spawn_producer<clock::time_point>([interval](writer<clock::time_point> w) {
        auto next = csp::now() + interval;
        while (true) {
            csp::sleep_until(next);
            if (!(w << csp::now())) break;
            next += interval;
        }
    });
}

}
