#pragma once

#include <csp/part/part.h>

#include <utility>

namespace csp::part {

namespace detail {

// Shared body for take_while/take_until.  `stop(t)` is the termination
// test; when it fires, `inclusive` decides whether the terminating
// element is emitted before the output closes.
template <typename T, typename Stop>
auto take_prefix(Stop stop, bool inclusive, char const* name) {
    return make_filter<T>(
        [stop = std::move(stop), inclusive, name](reader<T> in, writer<T> out) {
            internal::descr(name);
            for (T t; csp::alt(in >> t, ~out) == 0;) {
                bool const done = stop(t);
                if (done && !inclusive) return;
                if (!(out << std::move(t))) return;
                if (done) return;
            }
        });
}

}

// Forward elements while pred is true, then close output.
template <typename T, typename Pred>
auto take_while(Pred&& pred) {
    return detail::take_prefix<T>(
        [pred = std::forward<Pred>(pred)](T const& t) { return !pred(t); },
        false, "take_while");
}

}
