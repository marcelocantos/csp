#ifndef INCLUDED__csp__chain_h
#define INCLUDED__csp__chain_h

#include <csp/microthread.h>

#include <type_traits>
#include <utility>
#include <vector>

namespace csp {

    namespace chan {

        template <typename T, typename R,
                  typename = decltype(std::begin(std::declval<R>())->read())>
        auto chain(R rr, writer<T> w) {
            csp_descr("chan::chain");

            return [rr = std::move(rr), w = std::move(w)]{
                static Logger scope("chan/chain/scope");
                BRAC_SCOPE(scope, "chain", "%d readers", rr.size());

                static Logger log("chan/chain/log");

                for (auto r : rr) {
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

        template <typename T>
        auto chain(std::initializer_list<reader<T>> rr, writer<T> w) {
            return chain<T>(std::vector<reader<T>>(rr), w);
        }

        // Wire up an existing upstream reader, returning a downstream reader.
        template <typename T, typename R>
        reader<T> spawn_chain(R rr) {
            return spawn_producer<T>([rr = std::move(rr)](auto w) {
                return chain(rr, w);
            });
        }

        template <typename T>
        auto spawn_chain(std::initializer_list<reader<T>> rr) {
            return spawn_chain<T>(std::vector<reader<T>>(rr));
        }

    }

}

#endif // INCLUDED__csp__chain_h
