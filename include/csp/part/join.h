#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Block until all channels close. Drains and discards all values.
template <typename T>
void join(std::vector<reader<T>> inputs) {
    T t;
    std::vector<internal::ChanOp> chanops;
    chanops.reserve(inputs.size());
    for (auto& r : inputs)
        detail::fan_in_push_read(chanops, r, t);

    (void)detail::fan_in(inputs, chanops, t, /*base=*/0, /*watch_out=*/false,
                         [](T&) { return true; });
}

}
