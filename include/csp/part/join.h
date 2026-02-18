#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Block until all channels close. Drains and discards all values.
template <typename T>
void join(std::vector<reader<T>> inputs) {
    T t;
    std::vector<internal::ChanOp> chanops;
    for (auto& r : inputs) {
        chanops.push_back({internal::wait(r.internal_reader()), &t});
    }

    while (!inputs.empty()) {
        internal::AltMatch m;
        internal::alt_begin(&m, chanops.data(), chanops.size(), 0);
        if (m.src && m.dst)
            *static_cast<T*>(m.dst) = std::move(*static_cast<T*>(m.src));
        internal::alt_end(&m);

        if (m.result > 0) {
            // Value received — discard.
            continue;
        }
        // Reader died — remove it.
        size_t slot = static_cast<size_t>(~m.result - 1);
        inputs[slot] = std::move(inputs.back());
        inputs.pop_back();
        chanops[slot] = chanops.back();
        chanops.pop_back();
    }
}

}
