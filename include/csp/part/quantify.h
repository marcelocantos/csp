#pragma once

#include <csp/part/part.h>

#include <utility>

namespace csp::part {

namespace detail {

// Shared body for the short-circuiting quantifiers: emit `match` on the
// first element where bool(pred(t)) == match, or !match if the input
// exhausts without one.
template <typename T, typename Pred>
auto quantify(Pred pred, bool match, char const* name) {
    return make_filter<T, bool>(
        [pred = std::move(pred), match, name](reader<T> in, writer<bool> out) {
            internal::descr(name);
            for (T t; csp::alt(in >> t, ~out) == 0;) {
                if (bool(pred(t)) == match) {
                    out << match;
                    return;
                }
            }
            out << !match;
        });
}

}

// Short-circuiting existential quantifier.
// Emits true on first match, or false if input exhausts without a match.
template <typename T, typename Pred>
auto any_of(Pred&& pred) {
    return detail::quantify<T>(std::forward<Pred>(pred), true, "any_of");
}

// Short-circuiting universal quantifier.
// Emits false on first non-match, or true if all elements match.
template <typename T, typename Pred>
auto all_of(Pred&& pred) {
    return detail::quantify<T>(std::forward<Pred>(pred), false, "all_of");
}

}
