#pragma once

#include <csp/csp.h>

namespace csp {

        // keepalive: is never used, but when its writer dies, killswitch dies.
        template <typename A>
        auto killswitch(reader<A> in, writer<A> out, reader<> keepalive) {
            return [in = std::move(in), out = std::move(out), keepalive = std::move(keepalive)]{
                internal::descr("killswitch");

                for (A a; prialt(~keepalive, ~out, in >> a) > 0 && prialt(~keepalive, out << a) > 0;) { }
            };
        }

        // Wire up an existing downstream writer, returning an upstream writer.
        template <typename T>
        writer<T> spawn_killswitch(writer<T> w, reader<> keepalive) {
            return spawn_consumer<T>([w = std::move(w), keepalive = std::move(keepalive)](auto && r) mutable {
                killswitch(std::move(r), std::move(w), std::move(keepalive))();
            });
        }

        // Wire up an existing upstream reader, returning a downstream reader.
        template <typename T>
        reader<T> spawn_killswitch(reader<T> r, reader<> keepalive) {
            return spawn_producer<T>([r = std::move(r), keepalive = std::move(keepalive)](auto && w) mutable {
                killswitch(std::move(r), std::move(w), std::move(keepalive))();
            });
        }

        template <typename T>
        chan<T> spawn_killswitch(reader<> keepalive) {
            return spawn_filter<T>([keepalive = std::move(keepalive)](auto && r, auto && w) mutable {
                killswitch(std::move(r), std::move(w), std::move(keepalive))();
            });
        }

}
