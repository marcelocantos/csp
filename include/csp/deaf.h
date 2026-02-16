#ifndef INCLUDED__csp__deaf_h
#define INCLUDED__csp__deaf_h

#include <csp/microthread.h>

namespace csp {

        template <typename T>
        auto deaf(reader<T> in) {
            return [in = std::move(in)]{
                csp_descr("deaf");

                static Logger scope("chan/deaf/scope");
                BRAC_SCOPE(scope, "deaf", "");

                alt(~in);
            };
        }

        template <typename T>
        writer<T> spawn_deaf() {
            return spawn_consumer<T>([](auto r) mutable {
                deaf(std::move(r))();
            });
        }

}

#endif // INCLUDED__csp__deaf_h
