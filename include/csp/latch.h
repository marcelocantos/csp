#ifndef INCLUDED__csp__latch_h
#define INCLUDED__csp__latch_h

#include "mute.h"

namespace csp {

        template <typename T>
        auto latch(reader<T> in, writer<T> out) {
            return [in = std::move(in), out = std::move(out)]{
                csp_descr("latch");

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
            return spawn_consumer<T>([out = std::move(out)](auto in) mutable {
                latch(std::move(in), std::move(out))();
            });
        }

        // Wire up an existing upstream reader, returning a downstream reader.
        template <typename T>
        reader<T> spawn_latch(reader<T> in) {
            return spawn_producer<T>([in = std::move(in)](auto out) mutable {
                latch(std::move(in), std::move(out))();
            });
        }

        template <typename T>
        chan<T> spawn_latch() {
            return spawn_filter<T>([](auto in, auto out) mutable {
                latch(std::move(in), std::move(out))();
            });
        }

}
#endif // INCLUDED__csp__latch_h
