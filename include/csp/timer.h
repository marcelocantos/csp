#ifndef INCLUDED__csp__timer_h
#define INCLUDED__csp__timer_h

#include <csp/microthread.h>

#include <chrono>

namespace csp {

    using clock = std::chrono::steady_clock;

    // Block the current microthread until the given deadline.
    inline void sleep_until(clock::time_point tp) {
        csp_sleep_until(tp.time_since_epoch().count());
    }

    // Block the current microthread for the given duration.
    inline void sleep(clock::duration d) {
        sleep_until(clock::now() + d);
    }

    // Return a reader that fires once after the given duration.
    inline reader<> after(clock::duration d) {
        return spawn_producer<poke_t>([d](writer<> w) {
            csp::sleep(d);
            w << poke;
        });
    }

    // Return a reader that fires repeatedly at the given interval,
    // delivering the current time. Uses absolute deadlines to prevent drift.
    inline reader<clock::time_point> tick(clock::duration interval) {
        return spawn_producer<clock::time_point>([interval](writer<clock::time_point> w) {
            auto next = clock::now() + interval;
            while (true) {
                csp::sleep_until(next);
                if (!(w << clock::now())) break;
                next += interval;
            }
        });
    }

}

#endif // INCLUDED__csp__timer_h
