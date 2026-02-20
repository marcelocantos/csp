#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Dynamic-width zip: read one element from each of N inputs in lockstep,
// emit as vector<T>. Stops when any input dies or output dies.
template <typename T>
auto transpose(std::vector<reader<T>> inputs) {
    return make_producer<std::vector<T>>(
        [inputs = std::move(inputs)](writer<std::vector<T>> out) mutable {
            internal::descr("transpose");
            size_t n = inputs.size();
            for (;;) {
                std::vector<T> row;
                row.reserve(n);
                for (auto& r : inputs) {
                    T t;
                    if (csp::alt(r >> t, ~out) != 0) return;
                    row.push_back(std::move(t));
                }
                if (!(out << std::move(row))) return;
            }
        });
}

}
