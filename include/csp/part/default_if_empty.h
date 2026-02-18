#pragma once

#include <csp/part/part.h>

namespace csp::part {

// If input closes without producing any value, emit a default value.
// Otherwise, pass all values through unchanged.
template <typename T>
auto default_if_empty(T def) {
    return make_filter<T>([def = std::move(def)](reader<T> in, writer<T> out) {
        internal::descr("default_if_empty");
        bool any = false;
        for (T t; csp::alt(in >> t, ~out) == 0;) {
            any = true;
            if (!(out << std::move(t))) return;
        }
        if (!any) {
            out << def;
        }
    });
}

}
