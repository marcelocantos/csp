#pragma once

#include <csp/part/part.h>
#include <csp/timer.h>

namespace csp::part {

// Close output if no value arrives within duration d.
// Timer resets on each value. Values are forwarded unchanged.
template <typename T>
auto timeout(csp::clock::duration d) {
    return make_filter<T>([d](reader<T> in, writer<T> out) {
        internal::descr("timeout");
        auto timer = csp::after(d);

        for (;;) {
            T t;
            poke_t p;
            switch (csp::alt(in >> t, timer >> p, ~out)) {
            case 1:  // Value arrived — forward and reset timer.
                if (!(out << std::move(t))) return;
                timer = csp::after(d);
                break;
            case 2:  // Timeout fired — close.
                return;
            default:
                return;
            }
        }
    });
}

}
