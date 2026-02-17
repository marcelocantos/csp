#pragma once

#include <csp/part/part.h>
#include <csp/timer.h>

namespace csp::part {

// On each trigger, emit the most recent value from source.
// After source dies, keeps emitting the last latched value on each trigger
// until the trigger stream dies.
template <typename T, typename Trigger = poke_t>
auto sample(reader<T> source, reader<Trigger> trigger) {
    return make_producer<T>(
        [source = std::move(source), trigger = std::move(trigger)]
        (writer<T> out) mutable {
            internal::descr("sample");
            T latest;
            bool has_value = false;
            Trigger trig;

            for (;;) {
                switch (csp::alt(source >> latest, trigger >> trig, ~out)) {
                case 1:  // Source value — latch.
                    has_value = true;
                    break;
                case 2:  // Trigger — emit latest if any.
                    if (has_value && !(out << latest)) return;
                    break;
                case -1:  // Source died — emit on remaining triggers.
                    if (has_value) {
                        Trigger tr;
                        while (csp::alt(trigger >> tr, ~out) == 1) {
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
