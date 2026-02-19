#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Fold a channel to a single value. Consumes the entire input stream,
// then emits the final accumulator as a single output element.
// Composable via | — use .spawn().single() for terminal extraction.
template <typename T, typename S, typename F>
auto reduce(S init, F&& f) {
    return make_filter<T, S>(
        [init = std::move(init), f = std::forward<F>(f)]
        (reader<T> in, writer<S> out) {
            internal::descr("reduce");
            S acc = init;
            for (T t; csp::alt(in >> t, ~out) == 0;)
                acc = f(std::move(acc), std::move(t));
            out << std::move(acc);
        });
}

}
