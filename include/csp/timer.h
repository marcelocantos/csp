#pragma once

#include <csp/csp.h>
#include <csp/dynamic.h>

#include <chrono>
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
    std::priority_queue<Entry, std::vector<Entry>,
                        std::greater<Entry>> pending_;
    void fire_expired();

public:
    explicit fake_clock(time_point start = time_point{});

    time_point now() const override { return current_; }
    void sleep_until(time_point tp) override;

    bool has_pending() const { return !pending_.empty(); }

    // Advance time and fire expired timers.
    void advance(duration d);

    // Jump to next pending deadline. Returns false if no timers pending.
    bool advance_to_next();

    // Run scheduler loop with auto-advance until no work remains.
    void run();

    // Run scheduler until no imps are runnable (don't advance time).
    void run_until_idle();

    fake_clock(fake_clock const&) = delete;
    fake_clock& operator=(fake_clock const&) = delete;
};

extern dynamic<clock_source*> clock;

// Current time.
inline time_point now() {
    return (*clock)->now();
}

// Block the current imp until the given deadline.
inline void sleep_until(time_point tp) {
    (*clock)->sleep_until(tp);
}

// Block the current imp for the given duration.
inline void sleep(duration d) {
    sleep_until(csp::now() + d);
}

// Return a reader that fires once after the given duration,
// delivering the current time.
inline reader<time_point> after(duration d) {
    return spawn_producer<time_point>([d](writer<time_point> w) {
        csp::sleep(d);
        w << csp::now();
    });
}

// Return a reader that fires repeatedly at the given interval,
// delivering the current time. Uses absolute deadlines to prevent drift.
inline reader<time_point> tick(duration interval) {
    return spawn_producer<time_point>([interval](writer<time_point> w) {
        auto next = csp::now() + interval;
        while (true) {
            csp::sleep_until(next);
            if (!(w << csp::now())) break;
            next += interval;
        }
    });
}

}
