#pragma once

#include <csp/part/part.h>

#include <stdexcept>
#include <vector>

namespace csp::part {

// Read from whichever source responds first, discard the rest.
// Blocks the calling microthread until a value is available.
// Throws std::runtime_error if all readers close without producing a value.
template <typename T>
T first_wins(std::vector<reader<T>> inputs) {
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

        if (m.result >= 0) {
            return std::move(t);
        }
        // Reader died — remove it.
        size_t slot = static_cast<size_t>(~m.result);
        inputs[slot] = std::move(inputs.back());
        inputs.pop_back();
        chanops[slot] = chanops.back();
        chanops.pop_back();
    }

    throw std::runtime_error("first_wins: all readers closed without producing a value");
}

}
