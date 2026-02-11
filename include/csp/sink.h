#ifndef INCLUDED__csp__sink_h
#define INCLUDED__csp__sink_h

#include <csp/microthread.h>

namespace csp {

    namespace chan {

        template <typename A, typename F>
        auto sink(reader<A> in, F f) {
            return [in = std::move(in), f = std::move(f)] {
                csp_descr("chan::sink");

                static Logger scope("chan/sink/scope");
                BRAC_SCOPE(scope, "sink", "");

                for (auto a : in) {
                    f(a);
                }
            };
        }

        template <typename T, typename F>
        writer<T> spawn_sink(F f) {
            return spawn_consumer<T>([f = std::move(f)](auto && r) {
                sink(r, f)();
            });
        }

        template <typename T>
        writer<T> spawn_sinkhole(T & t) {
            return spawn_sink<T>([t = &t](T a) {
                *t = a;
            });
        }

    }

}

#endif // INCLUDED__csp__sink_h
