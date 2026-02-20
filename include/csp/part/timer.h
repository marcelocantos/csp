#pragma once

#include <csp/csp.h>
#include <csp/timer.h>

namespace csp::part {

// Each duration read from control becomes the next sleep interval.
// Emits the actual fire time after each sleep.
inline reader<time_point> timer(reader<duration> control) {
    return spawn_producer<time_point>(
        [control = std::move(control)](writer<time_point> out) mutable {
            internal::descr("timer");
            for (duration d; control >> d;) {
                csp::sleep(d);
                if (!(out << csp::now())) return;
            }
        });
}

// Each time_point read from control becomes the next absolute deadline.
// Emits the actual fire time after each sleep.
inline reader<time_point> timer(reader<time_point> control) {
    return spawn_producer<time_point>(
        [control = std::move(control)](writer<time_point> out) mutable {
            internal::descr("timer");
            for (time_point tp; control >> tp;) {
                csp::sleep_until(tp);
                if (!(out << csp::now())) return;
            }
        });
}

}
