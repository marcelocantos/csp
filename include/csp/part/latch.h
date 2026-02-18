#pragma once

#include <csp/part/part.h>
#include "mute.h"

namespace csp::part {

// Hold and serve the most recent value.
// While the writer is alive, each read returns the latest written value.
// After the writer dies, the last value is served repeatedly.
template <typename T>
inline auto const latch = make_filter<T>([](reader<T> in, writer<T> out) {
    internal::descr("latch");

    T t;
    if (prialt(~out, in >> t) >= 0) {
        while (prialt(in >> t, out << t) >= 0) { }
        while (out << t) { }
    }
});

}
