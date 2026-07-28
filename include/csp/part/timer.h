#pragma once

#include <csp/csp.h>
#include <csp/timer.h>

#include <utility>

namespace csp::part {

namespace detail {

// Shared body for the two timer overloads, parameterised on the sleep
// call (relative csp::sleep vs absolute csp::sleep_until).
template <typename C, typename SleepFn>
reader<time_point> timer_loop(reader<C> control, SleepFn sleep_fn) {
    return spawn_producer<time_point>(
        [control = std::move(control),
         sleep_fn = std::move(sleep_fn)](writer<time_point> out) mutable {
            internal::descr("timer");
            for (C c; control >> c;) {
                sleep_fn(c);
                if (!(out << csp::now())) return;
            }
        });
}

}

// Eager by design: timer returns live endpoints (a reader<time_point>
// backed by an already-spawned imp), not a lazy make_* part.  The lazy
// convention (see part.h) does not apply — converting would be a
// user-visible breaking change.

// Each duration read from control becomes the next sleep interval.
// Emits the actual fire time after each sleep.
inline reader<time_point> timer(reader<duration> control) {
    return detail::timer_loop(std::move(control),
                              [](duration d) { csp::sleep(d); });
}

// Each time_point read from control becomes the next absolute deadline.
// Emits the actual fire time after each sleep.
inline reader<time_point> timer(reader<time_point> control) {
    return detail::timer_loop(std::move(control),
                              [](time_point tp) { csp::sleep_until(tp); });
}

}
