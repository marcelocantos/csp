#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Fold a channel to a single value. Blocks the calling microthread until
// the input is exhausted, then returns the final accumulator.
template <typename T, typename S, typename F>
S reduce(reader<T> in, S init, F f) {
    for (T t; in >> t;)
        init = f(std::move(init), t);
    return init;
}

}
