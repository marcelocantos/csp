#pragma once

#include <csp/csp.h>

namespace csp {

        template <typename A, typename F>
        auto sink(reader<A> in, F f) {
            return [in = std::move(in), f = std::move(f)] {
                csp_descr("sink");

                static Logger scope("chan/sink/scope");
                BRAC_SCOPE(scope, "sink", "");

                for (auto a : in) {
                    f(a);
                }
            };
        }

        template <typename T, typename F>
        writer<T> spawn_sink(F f) {
            return spawn_consumer<T>([f = std::move(f)](auto && r) mutable {
                sink(std::move(r), std::move(f))();
            });
        }

        template <typename T>
        writer<T> spawn_sinkhole(T & t) {
            return spawn_sink<T>([t = &t](T a) {
                *t = a;
            });
        }

}
