#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// N-way partition: route each element to one of n outputs based on a
// classifier f(T) → size_t. Elements mapping to index i go to outputs[i].
// If f returns an out-of-range index, the element is dropped.
// Dead outputs are skipped; closes when input is exhausted or all outputs die.
template <typename T, typename F>
auto partition(reader<T> in, size_t n, F f) {
    std::vector<writer<T>> writers;
    std::vector<reader<T>> readers;
    writers.reserve(n);
    readers.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        chan<T> ch;
        writers.push_back(std::move(ch.w));
        readers.push_back(std::move(ch.r));
    }

    csp::spawn([in = std::move(in), writers = std::move(writers),
                f = std::move(f)]() mutable {
        internal::descr("partition");

        size_t live = writers.size();
        for (T t; live > 0 && (in >> t);) {
            size_t idx = f(t);
            if (idx >= writers.size()) continue;
            if (!writers[idx]) continue; // already dead
            if (!(writers[idx] << std::move(t))) {
                writers[idx] = {};
                --live;
            }
        }
    });

    return readers;
}

// Binary partition: route elements by predicate. Elements where pred(t) is
// true go to outputs[1], false to outputs[0].
template <typename T, typename Pred>
auto partition(reader<T> in, Pred pred) {
    return partition<T>(std::move(in), 2,
        [pred = std::move(pred)](const T& t) -> size_t {
            return int(pred(t));
        });
}

}
