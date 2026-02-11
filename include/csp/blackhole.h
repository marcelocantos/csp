#ifndef INCLUDED__csp__blackhole_h
#define INCLUDED__csp__blackhole_h

#include <csp/microthread.h>

namespace csp {

    namespace chan {

        template <typename T>
        auto blackhole(reader<T> in) {
            return [=]{
                for (T _; in >> _;) { }
            };
        }

        template <typename T>
        writer<T> spawn_blackhole() {
            return spawn_consumer<T>([](auto && r) {
                blackhole(r)();
            });
        }

    }

}

#endif // INCLUDED__csp__blackhole_h
