#ifndef INCLUDED__csp__killswitch_h
#define INCLUDED__csp__killswitch_h

#include <csp/microthread.h>

namespace csp {

    namespace chan {

        // keepalive: is never used, but when its writer dies, killswitch dies.
        template <typename A>
        auto killswitch(reader<A> in, writer<A> out, reader<> keepalive) {
            csp_descr("chan::killswitch");

            return [in = std::move(in), out = std::move(out), keepalive = std::move(keepalive)]{
                for (A a; prialt(~keepalive, ~out, in >> a) > 0 && prialt(~keepalive, out << a) > 0;) { }
            };
        }

        // Wire up an existing downstream writer, returning an upstream writer.
        template <typename T>
        writer<T> spawn_killswitch(writer<T> w, reader<> keepalive) {
            return spawn_consumer<T>([w = std::move(w), keepalive = std::move(keepalive)](auto r) {
                killswitch(r, w, keepalive)();
            });
        }

        // Wire up an existing upstream reader, returning a downstream reader.
        template <typename T>
        reader<T> spawn_killswitch(reader<T> r, reader<> keepalive) {
            return spawn_producer<T>([r = std::move(r), keepalive = std::move(keepalive)](auto w) {
                killswitch(r, w, keepalive)();
            });
        }

        template <typename T>
        channel<T> spawn_killswitch(reader<> keepalive) {
            return spawn_filter<T>([keepalive = std::move(keepalive)](auto r, auto w) {
                killswitch(r, w, keepalive)();
            });
        }
        
    }
    
}

#endif // INCLUDED__csp__killswitch_h
