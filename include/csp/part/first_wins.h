#pragma once

#include <csp/part/part.h>

#include <stdexcept>
#include <vector>

namespace csp::part {

// Read from whichever source responds first, discard the rest.
// Blocks the calling imp until a value is available.
// Throws std::runtime_error if all readers close without producing a value.
template <typename T>
T first_wins(std::vector<reader<T>> inputs) {
    T t{};
    std::vector<internal::ChanOp> chanops;
    chanops.reserve(inputs.size());
    for (auto& r : inputs)
        detail::fan_in_push_read(chanops, r, t);

    bool got = false;
    T result{};
    (void)detail::fan_in(inputs, chanops, t, /*base=*/0, /*watch_out=*/false,
                         [&](T& v) {
                             result = std::move(v);
                             got = true;
                             return false;  // stop after first value
                         });
    if (!got)
        throw std::runtime_error(
            "first_wins: all readers closed without producing a value");
    return result;
}

}
