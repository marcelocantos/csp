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
            // Slot 0: death-watch on output.
            chanops.push_back({internal::wait_dead(out.internal_writer()), nullptr});
            // Slots 1..N: reads from each input.
            for (auto& r : inputs) {
                chanops.push_back({internal::wait(r.internal_reader()), &t});
            }

            while (!inputs.empty()) {
                internal::AltMatch m;
                internal::alt_begin(&m, chanops.data(), chanops.size(), 0);
                if (m.src && m.dst)
                    *static_cast<T*>(m.dst) = std::move(*static_cast<T*>(m.src));
                internal::alt_end(&m);

                if (m.result == -1) {
                    // Output peer died.
                    return;
                } else if (m.result > 0) {
                    // Read succeeded — forward to output.
                    if (!(out << std::move(t))) return;
                } else {
                    // A reader died. Slot index = -result - 1 (1-based).
                    size_t slot = static_cast<size_t>(-m.result - 1);
                    size_t i = slot - 1; // 0-based index into inputs.
                    inputs[i] = std::move(inputs.back());
                    inputs.pop_back();
                    chanops[slot] = chanops.back();
                    chanops.pop_back();
                }
            }
        });
}

}
