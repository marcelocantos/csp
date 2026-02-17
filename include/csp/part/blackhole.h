#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Consume and discard all values from a channel.
template <typename T>
inline auto const blackhole = make_consumer<T>([](reader<T> in) {
    for (T _; in >> _;) { }
});

}
