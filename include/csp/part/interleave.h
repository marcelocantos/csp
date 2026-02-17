#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Interleave N inputs into one output in strict round-robin order.
// Deterministic dual of round_robin. Reads from each input in turn.
// Dead inputs are removed; closes when all inputs are exhausted or output dies.
template <typename T>
auto interleave(std::vector<reader<T>> inputs) {
    return make_producer<T>(
        [inputs = std::move(inputs)](writer<T> out) mutable {
            internal::descr("interleave");

            size_t i = 0;
            while (!inputs.empty()) {
                T t;
                if (!(inputs[i] >> t)) {
                    // Input died — remove it.
                    inputs[i] = std::move(inputs.back());
                    inputs.pop_back();
                    if (!inputs.empty() && i >= inputs.size()) i = 0;
                    continue;
                }
                if (!(out << std::move(t))) return;
                i = (i + 1) % inputs.size();
            }
        });
}

}
