#pragma once

#include <csp/part/part.h>

#include <utility>

namespace csp::part {

// Consume all values into an output iterator.
template <typename T, typename Iter>
auto collect(Iter it) {
    return make_consumer<T>([it](reader<T> in) mutable {
        internal::descr("collect");
        for (T v; in >> v;) *it++ = std::move(v);
    });
}

}
