#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Non-deterministic merge of N readers into one.
// Reads from whichever input is ready first. When a reader dies it is
// removed; output closes when all inputs are exhausted or the output dies.
template <typename T>
auto merge(std::vector<reader<T>> inputs) {
    return make_producer<T>(
        [inputs = std::move(inputs)](writer<T> out) mutable {
            internal::descr("merge");

            T t;
            std::vector<internal::ChanOp> chanops;
            chanops.reserve(inputs.size() + 1);
            // Slot 0: death-watch on output.
            chanops.push_back(detail::fan_in_out_dead(out));
            // Slots 1..N: reads from each input.
            for (auto& r : inputs)
                detail::fan_in_push_read(chanops, r, t);

            // base=1 (slot 0 is out-death); stop on out death or write fail.
            (void)detail::fan_in(inputs, chanops, t, /*base=*/1, /*watch_out=*/true,
                                 [&](T& v) { return bool(out << std::move(v)); });
        });
}

}
