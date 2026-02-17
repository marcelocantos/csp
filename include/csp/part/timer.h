#pragma once

#include <csp/csp.h>
#include <csp/timer.h>

namespace csp::part {

// Each duration read from control becomes the next sleep interval.
// Emits the actual fire time after each sleep.
inline reader<clock::time_point> timer(reader<clock::duration> control) {
    return spawn_producer<clock::time_point>(
        [control = std::move(control)](writer<clock::time_point> out) mutable {
            internal::descr("timer");
            for (clock::duration d; control >> d;) {
                csp::sleep(d);
                if (!(out << clock::now())) return;
            }
        });
}

// Each time_point read from control becomes the next absolute deadline.
// Emits the actual fire time after each sleep.
inline reader<clock::time_point> timer(reader<clock::time_point> control) {
    return spawn_producer<clock::time_point>(
        [control = std::move(control)](writer<clock::time_point> out) mutable {
            internal::descr("timer");
            for (clock::time_point tp; control >> tp;) {
                csp::sleep_until(tp);
                if (!(out << clock::now())) return;
            }
        });
}

}
