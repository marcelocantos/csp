#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Group consecutive elements where pred(prev, curr) is true.
// Emits each group as a vector. Flushes the final group on input exhaustion.
template <typename T, typename F>
auto chunk_by(F&& f) {
    return make_filter<T, std::vector<T>>(
        [f = std::forward<F>(f)]
        (reader<T> in, writer<std::vector<T>> out) {
            internal::descr("chunk_by");
            std::vector<T> chunk;
            for (T t; csp::alt(in >> t, ~out) == 0;) {
                if (!chunk.empty() && !f(chunk.back(), t)) {
                    if (!(out << std::move(chunk))) return;
                    chunk.clear();
                }
                chunk.push_back(std::move(t));
            }
            if (!chunk.empty())
                out << std::move(chunk);
        });
}

}
