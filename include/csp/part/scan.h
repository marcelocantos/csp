#pragma once

#include <csp/part/part.h>

#include <utility>

namespace csp::part {

// Running fold/accumulator.
// Starts with init, applies acc = f(acc, value) for each input, and emits
// the new accumulator after every step.
template <typename T, typename S, typename F>
auto scan(S init, F&& f) {
    return make_filter<T, S>(
        [init = std::move(init), f = std::forward<F>(f)]
        (reader<T> in, writer<S> out) {
            internal::descr("scan");
            S acc = init;
            for (T t; csp::alt(in >> t, ~out) == 0;) {
                acc = f(std::move(acc), std::move(t));
                if (!(out << acc)) return;
            }
        });
}

}
