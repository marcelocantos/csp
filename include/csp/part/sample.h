#pragma once

#include <csp/part/part.h>
#include <csp/timer.h>

namespace csp::part {

// On each trigger, emit the most recent value from source.
// After source dies, keeps emitting the last latched value on each trigger
// until the trigger stream dies.
// Optional dead_letter: overwritten latched values are written here instead of
// discarded.
template <typename T, typename Trigger = poke_t>
auto sample(reader<T> source, reader<Trigger> trigger, writer<T> dead_letter = {}) {
    return make_producer<T>(
        [source = std::move(source), trigger = std::move(trigger),
         dead_letter = std::move(dead_letter)]
        (writer<T> out) mutable {
            internal::descr("sample");
            T latest;
            bool has_value = false;
            Trigger trig;
            T next;

            for (;;) {
                switch (csp::alt(source >> next, trigger >> trig, ~out)) {
                case 0:  // Source value — latch.
                    if (dead_letter && has_value) dead_letter << std::move(latest);
                    latest = std::move(next);
                    has_value = true;
                    break;
                case 1:  // Trigger — emit latest if any.
                    if (has_value && !(out << latest)) return;
                    break;
                case ~0:  // Source died — emit on remaining triggers.
                    if (has_value) {
                        Trigger tr;
                        while (csp::alt(trigger >> tr, ~out) == 0) {
                            if (!(out << latest)) return;
                        }
                    }
                    return;
                default:
                    return;
                }
            }
        });
}

}
