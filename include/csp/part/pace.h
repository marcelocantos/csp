#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Rate-limited passthrough via external trigger. All values are preserved
// (backpressure, no drops). The first value passes immediately; subsequent
// values wait for a trigger before emission.
// Use with tick(d) for periodic pacing: pace<int>(tick(100ms)).
template <typename T, typename Trigger = poke_t>
auto pace(reader<Trigger> trigger) {
    return make_filter<T>([trigger = std::move(trigger)](reader<T> in, writer<T> out) mutable {
        internal::descr("pace");
        T t;
        // First value passes immediately.
        if (csp::alt(in >> t, ~out) != 0) return;
        if (!(out << std::move(t))) return;

        // Subsequent values wait for trigger before emission.
        Trigger trig;
        while (csp::alt(in >> t, ~out) == 0) {
            if (csp::alt(trigger >> trig, ~out) != 0) return;
            if (!(out << std::move(t))) return;
        }
    });
}

}
