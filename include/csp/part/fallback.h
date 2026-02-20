#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Sequential failover: try each reader in order. If a reader produces at
// least one value, drain it fully and stop. If it closes without producing
// anything, try the next. Output closes empty if all inputs close empty.
template <typename T>
auto fallback(std::vector<reader<T>> inputs) {
    return make_producer<T>(
        [inputs = std::move(inputs)](writer<T> out) mutable {
            internal::descr("fallback");
            for (auto& r : inputs) {
                bool produced = false;
                for (T t; csp::alt(r >> t, ~out) == 0;) {
                    produced = true;
                    if (!(out << std::move(t))) return;
                }
                if (produced) return;
            }
        });
}

}
