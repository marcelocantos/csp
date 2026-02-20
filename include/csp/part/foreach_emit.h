#pragma once

#include <csp/part/part.h>

#include <utility>

namespace csp::part {

// Generalized scan with separate state update and extraction phases.
// state = update(state, input), then emits extract(state) each step.
template <typename T, typename S, typename U, typename Update, typename Extract>
auto foreach_emit(S init, Update&& update, Extract&& extract) {
    return make_filter<T, U>(
        [init = std::move(init),
         update = std::forward<Update>(update),
         extract = std::forward<Extract>(extract)]
        (reader<T> in, writer<U> out) {
            internal::descr("foreach_emit");
            S state = init;
            for (T t; csp::alt(in >> t, ~out) == 0;) {
                state = update(std::move(state), std::move(t));
                if (!(out << extract(state))) return;
            }
        });
}

}
