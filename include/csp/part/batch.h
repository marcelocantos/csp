#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Collect n elements into a vector and emit it.
// Repeats until the input or output dies. Any partial batch remaining
// when the input closes is flushed as a shorter vector.
template <typename T>
auto batch(size_t n) {
    return make_filter<T, std::vector<T>>(
        [n](reader<T> in, writer<std::vector<T>> out) {
            internal::descr("batch");
            std::vector<T> buf;
            buf.reserve(n);
            for (T t; csp::alt(in >> t, ~out) == 0;) {
                buf.push_back(std::move(t));
                if (buf.size() == n) {
                    if (!(out << std::move(buf))) return;
                    buf.clear();
                    buf.reserve(n);
                }
            }
            if (!buf.empty()) {
                out << std::move(buf);
            }
        });
}

}
