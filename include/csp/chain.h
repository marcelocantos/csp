#ifndef INCLUDED__csp__chain_h
#define INCLUDED__csp__chain_h

#include <csp/microthread.h>

#include <type_traits>
#include <utility>
#include <vector>

namespace csp {

    template <typename T, typename R,
              typename = decltype(std::begin(std::declval<R>())->read())>
    auto chain(R rr, writer<T> w) {
        csp_descr("chain");

        return [rr = std::move(rr), w = std::move(w)]{
            static Logger scope("chan/chain/scope");
            BRAC_SCOPE(scope, "chain", "%d readers", rr.size());

            static Logger log("chan/chain/log");

            for (auto & r : rr) {
                for (T n; csp::alt(r >> n, ~w) == 1;) {
                    CSP_LOG(log, "loop");
                    if (!(w << n)) {
                        break;
                    }
                }
                CSP_LOG(log, "next in");
            }
        };
    }

    // Wire up an existing upstream reader, returning a downstream reader.
    template <typename T, typename R>
    reader<T> spawn_chain(R rr) {
        return spawn_producer<T>([rr = std::move(rr)](auto w) mutable {
            chain(std::move(rr), std::move(w))();
        });
    }

}

#endif // INCLUDED__csp__chain_h
