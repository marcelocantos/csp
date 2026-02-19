#pragma once

#include <csp/part/part.h>
#include <csp/timer.h>

namespace csp::part {

// Suppress rapid values; emit only after a quiet period elapses.
// When input closes with a pending value, emits it immediately.
// Optional dead_letter: superseded pending values are written here instead of
// discarded.
template <typename T>
auto debounce(csp::clock::duration d, writer<T> dead_letter = {}) {
    return make_filter<T>([d, dead_letter = std::move(dead_letter)](reader<T> in, writer<T> out) mutable {
        internal::descr("debounce");
        T pending;
        reader<clock::time_point> timer;

        for (;;) {
            if (!timer) {
                // No pending value — wait for input.
                if (csp::alt(in >> pending, ~out) != 0) return;
                timer = csp::after(d);
            } else {
                // Pending value — wait for input, timer, or death.
                T next;
                clock::time_point tp;
                switch (csp::alt(in >> next, timer >> tp, ~out)) {
                case 0:  // New value supersedes pending.
                    if (dead_letter) dead_letter << std::move(pending);
                    pending = std::move(next);
                    timer = csp::after(d);
                    break;
                case 1:  // Timer fired — emit.
                    if (!(out << std::move(pending))) return;
                    timer = {};
                    break;
                case ~0:  // Input died — emit pending, done.
                    out << std::move(pending);
                    return;
                default:  // Output or timer died.
                    return;
                }
            }
        }
    });
}

}
