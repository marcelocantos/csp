#ifndef INCLUDED__csp__tee_h
#define INCLUDED__csp__tee_h

#include <csp/microthread.h>

#include <csp/internal/on_scope_exit.h>

namespace csp {

    namespace chan {

        // Tee successfully delivered messages to a side channel.
        // Keep going after ~tee.
        template <typename T>
        auto tee(reader<T> in, writer<T> out, writer<T> tee) {
            return [=]{
                csp_descr("chan::tee");

                static Logger scope("chan/tee/scope");
                BRAC_SCOPE(scope, "tee", "");

                static Logger log("chan/tee/log");

                for (T t; prialt(~out, in >> t) > 0 && out << t && tee << t;) { }
                for (T t; prialt(~out, in >> t) > 0 && out << t;) { }
            };
        }

        template <typename T>
        writer<T> spawn_tee(writer<T> out) {
            return spawn_consumer<T>([=](auto in) {
                tee(in, out)();
            });
        }

        template <typename T>
        reader<T> spawn_tee(reader<T> in) {
            return spawn_producer<T>([=](auto out) {
                tee(in, out)();
            });
        }

    }

}

#endif // INCLUDED__csp__tee_h
