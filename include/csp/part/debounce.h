#pragma once

#include <csp/part/part.h>
#include <csp/timer.h>

namespace csp::part {

// Suppress rapid values; emit only after a quiet period elapses.
// When input closes with a pending value, emits it immediately.
template <typename T>
auto debounce(csp::clock::duration d) {
    return make_filter<T>([d](reader<T> in, writer<T> out) {
        internal::descr("debounce");
        T pending;
        reader<> timer;

        for (;;) {
            if (!timer) {
                // No pending value — wait for input.
                if (csp::alt(in >> pending, ~out) != 1) return;
                timer = csp::after(d);
            } else {
                // Pending value — wait for input, timer, or death.
                poke_t p;
                switch (csp::alt(in >> pending, timer >> p, ~out)) {
                case 1:  // New value — restart timer.
                    timer = csp::after(d);
                    break;
                case 2:  // Timer fired — emit.
                    if (!(out << std::move(pending))) return;
                    timer = {};
                    break;
                case -1:  // Input died — emit pending, done.
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
