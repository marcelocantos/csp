#pragma once

#include <csp/part/part.h>

namespace csp::part {

    template <typename T>
    inline auto const blackhole = make_consumer<T>([](reader<T> in) {
        for (T _; in >> _;) { }
    });

}
