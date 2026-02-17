#pragma once

#include <csp/part/part.h>

#include <deque>
#include <vector>

namespace csp::part {

// Sliding window that emits the full window as a vector on every input.
// Always slides in (emits partial windows during growth).
template <typename T>
auto window(size_t n) {
    return make_filter<T, std::vector<T>>(
        [n](reader<T> in, writer<std::vector<T>> out) {
            internal::descr("window");

            std::deque<T> win;

            for (T t; alt(in >> t, ~out) > 0;) {
                if (win.size() >= n)
                    win.pop_front();
                win.push_back(std::move(t));
                if (!(out << std::vector<T>(win.begin(), win.end()))) return;
            }
        });
}

}
