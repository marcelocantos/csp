#ifndef INCLUDED__csp__map_h
#define INCLUDED__csp__map_h

#include <csp/microthread.h>

namespace csp {

    namespace chan {

        template <typename A, typename B, typename F>
        auto map(reader<A> in, writer<B> out, F && f) {
            return [in = std::move(in), out = std::move(out), f]{
                csp_descr("chan::map");

                for (A a; alt(in >> a, ~out) > 0 && out << f(a);) { }
            };
        }

        // Wire up an existing downstream writer, returning an upstream writer.
        template <typename A, typename B, typename F>
        writer<A> spawn_map(writer<B> w, F && f) {
            return spawn_consumer<A>([w = std::move(w), f = std::move(f)](auto && r) {
                map(r, w, f)();
            });
        }

        // Wire up an existing upstream reader, returning a downstream reader.
        template <typename B, typename A, typename F>
        reader<B> spawn_map(reader<A> r, F && f) {
            return spawn_producer<B>([=, r = std::move(r), f = std::move(f)](auto && w) {
                map(r, w, f)();
            });
        }

        template <typename T, typename F>
        channel<T> spawn_map(F && f) {
            return spawn_filter<T>([=](auto && r, auto && w) {
                map(r, w, f)();
            });
        }

    }

}

#endif // INCLUDED__csp__map_h
