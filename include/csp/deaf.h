#ifndef INCLUDED__csp__deaf_h
#define INCLUDED__csp__deaf_h

#include <csp/microthread.h>

namespace csp {

    namespace chan {

        template <typename T>
        auto deaf(reader<T> in) {
            return [=]{
                csp_descr("chan::deaf");

                static Logger scope("chan/deaf/scope");
                BRAC_SCOPE(scope, "deaf", "");

                alt(~in);
            };
        }

        template <typename T>
        writer<T> spawn_deaf() {
            return spawn_consumer<T>([](auto r) {
                deaf(r)();
            });
        }

    }

}

#endif // INCLUDED__csp__deaf_h
