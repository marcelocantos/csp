#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Distribute input across N outputs in round-robin fashion.
// Deterministic dual of interleave. Each value goes to exactly one output.
// Dead outputs are removed; closes when input is exhausted or all outputs die.
template <typename T>
auto round_robin(reader<T> in, size_t n) {
    std::vector<writer<T>> writers;
    std::vector<reader<T>> readers;
    writers.reserve(n);
    readers.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        chan<T> ch;
        writers.push_back(std::move(ch.w));
        readers.push_back(std::move(ch.r));
    }

    csp::spawn([in = std::move(in), writers = std::move(writers)]() mutable {
        internal::descr("round_robin");

        size_t i = 0;
        for (T t; !writers.empty() && (in >> t);) {
            if (!(writers[i] << t)) {
                // Output died — remove and retry same value with next output.
                writers[i] = std::move(writers.back());
                writers.pop_back();
                if (writers.empty()) return;
                if (i >= writers.size()) i = 0;
                continue;
            }
            i = (i + 1) % writers.size();
        }
    });

    return readers;
}

}
