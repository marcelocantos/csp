#ifndef INCLUDED__csp__mute_h
#define INCLUDED__csp__mute_h

#include <csp/microthread.h>

namespace csp {

    namespace chan {

        template <typename T>
        auto mute(writer<T> out) {
            return [=]{
                csp_descr("chan::mute");

                alt(~out);
            };
        }

        template <typename T = poke_t>
        reader<T> spawn_mute() {
            return spawn_producer<T>([](auto && r) {
                mute(r)();
            });
        }

    }

}

#endif // INCLUDED__csp__mute_h
