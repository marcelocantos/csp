#pragma once

#include <csp/part/part.h>

#include <type_traits>
#include <utility>
#include <vector>

namespace csp::part {

// Concatenate multiple readers into a single sequential stream.
// Reads each input to exhaustion in order, then moves to the next.
template <typename T, typename R,
          typename = decltype(std::begin(std::declval<R>())->read())>
auto chain(R rr) {
    return make_producer<T>([rr = std::move(rr)](writer<T> w) {
        internal::descr("chain");

        static Logger scope("chan/chain/scope");
        BRAC_SCOPE(scope, "chain", "%d readers", rr.size());

        static Logger log("chan/chain/log");

        for (auto & r : rr) {
            for (T n; csp::alt(r >> n, ~w) == 1;) {
                CSP_LOG(log, "loop");
                if (!(w << std::move(n))) {
                    break;
                }
            }
            CSP_LOG(log, "next in");
        }
    });
}

}
