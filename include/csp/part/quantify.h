#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Short-circuiting existential quantifier.
// Emits true on first match, or false if input exhausts without a match.
template <typename T, typename Pred>
auto any_of(Pred&& pred) {
    return make_filter<T, bool>(
        [pred = std::forward<Pred>(pred)](reader<T> in, writer<bool> out) {
            internal::descr("any_of");
            for (T t; csp::alt(in >> t, ~out) == 0;) {
                if (pred(t)) {
                    out << true;
                    return;
                }
            }
            out << false;
        });
}

// Short-circuiting universal quantifier.
// Emits false on first non-match, or true if all elements match.
template <typename T, typename Pred>
auto all_of(Pred&& pred) {
    return make_filter<T, bool>(
        [pred = std::forward<Pred>(pred)](reader<T> in, writer<bool> out) {
            internal::descr("all_of");
            for (T t; csp::alt(in >> t, ~out) == 0;) {
                if (!pred(t)) {
                    out << false;
                    return;
                }
            }
            out << true;
        });
}

}
