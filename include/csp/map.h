#pragma once

#include <csp/csp.h>

namespace csp {

        template <typename A, typename B, typename F>
        auto map(reader<A> in, writer<B> out, F && f) {
            return [in = std::move(in), out = std::move(out), f]{
                internal::descr("map");

                for (A a; alt(in >> a, ~out) > 0 && out << f(a);) { }
            };
        }

        // Wire up an existing downstream writer, returning an upstream writer.
        template <typename A, typename B, typename F>
        writer<A> spawn_map(writer<B> w, F && f) {
            return spawn_consumer<A>([w = std::move(w), f = std::move(f)](auto && r) mutable {
                map(std::move(r), std::move(w), std::move(f))();
            });
        }

        // Wire up an existing upstream reader, returning a downstream reader.
        template <typename B, typename A, typename F>
        reader<B> spawn_map(reader<A> r, F && f) {
            return spawn_producer<B>([r = std::move(r), f = std::move(f)](auto && w) mutable {
                map(std::move(r), std::move(w), std::move(f))();
            });
        }

        template <typename T, typename F>
        chan<T> spawn_map(F && f) {
            return spawn_filter<T>([f = std::move(f)](auto && r, auto && w) mutable {
                map(std::move(r), std::move(w), std::move(f))();
            });
        }

}
