#ifndef INCLUDED__csp__blackhole_h
#define INCLUDED__csp__blackhole_h

#include <csp/microthread.h>

namespace csp {

        template <typename T>
        auto blackhole(reader<T> in) {
            return [in = std::move(in)]{
                for (T _; in >> _;) { }
            };
        }

        template <typename T>
        writer<T> spawn_blackhole() {
            return spawn_consumer<T>([](auto && r) mutable {
                blackhole(std::move(r))();
            });
        }

}

#endif // INCLUDED__csp__blackhole_h
