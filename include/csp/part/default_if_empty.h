#pragma once

#include <csp/part/part.h>

namespace csp::part {

// If input closes without producing any value, emit a default value.
// Otherwise, pass all values through unchanged.
template <typename T>
auto default_if_empty(T def) {
    return make_filter<T>([def = std::move(def)](reader<T> in, writer<T> out) {
        internal::descr("default_if_empty");
        // Prime with the first element (see part.h: prime, don't flag).
        T first;
        switch (csp::alt(in >> first, ~out)) {
        case 0:
            break;
        case ~0:  // Input closed empty — emit the default.
            out << def;
            return;
        default:  // Output died.
            return;
        }
        if (!(out << std::move(first))) return;
        for (T t; csp::alt(in >> t, ~out) == 0;) {
            if (!(out << std::move(t))) return;
        }
    });
}

}
