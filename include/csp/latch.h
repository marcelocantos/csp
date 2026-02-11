#ifndef INCLUDED__csp__latch_h
#define INCLUDED__csp__latch_h

#include "mute.h"

namespace csp {

    namespace chan {

        template <typename T>
        auto latch(reader<T> in, writer<T> out) {
            return [=]{
                csp_descr("chan::latch");

                T t;
                if (prialt(~out, in >> t) > 0) {
                    while (prialt(in >> t, out << t) > 0) { }
                    while (out << t) { }
                }
            };
        }

        //----------------------------------------------------------------
        // spawn_* overloads

        // Wire up an existing downstream writer, returning an upstream writer.
        template <typename T>
        writer<T> spawn_latch(writer<T> out) {
            return spawn_consumer<T>([=](auto in) {
                latch(in, out)();
            });
        }

        // Wire up an existing upstream reader, returning a downstream reader.
        template <typename T>
        reader<T> spawn_latch(reader<T> in) {
            return spawn_consumer<T>([=](auto out) {
                latch(in, out)();
            });
        }

        template <typename T>
        channel<T> spawn_latch() {
            return spawn_filter<T>([](auto in, auto out) {
                latch(in, out)();
            });
        }

    }

}
#endif // INCLUDED__csp__latch_h
