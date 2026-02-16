#ifndef INCLUDED__csp__tee_h
#define INCLUDED__csp__tee_h

#include <csp/microthread.h>

#include <csp/internal/on_scope_exit.h>

namespace csp {

        // Tee successfully delivered messages to a side channel.
        // Keep going after ~tee.
        template <typename T>
        auto tee(reader<T> in, writer<T> out, writer<T> tee) {
            return [in = std::move(in), out = std::move(out), tee = std::move(tee)]{
                csp_descr("tee");

                static Logger scope("chan/tee/scope");
                BRAC_SCOPE(scope, "tee", "");

                static Logger log("chan/tee/log");

                for (T t; prialt(~out, in >> t) > 0 && out << t && tee << t;) { }
                for (T t; prialt(~out, in >> t) > 0 && out << t;) { }
            };
        }

        template <typename T>
        writer<T> spawn_tee(writer<T> out) {
            return spawn_consumer<T>([out = std::move(out)](auto && in) mutable {
                tee(std::move(in), std::move(out))();
            });
        }

        template <typename T>
        reader<T> spawn_tee(reader<T> in) {
            return spawn_producer<T>([in = std::move(in)](auto && out) mutable {
                tee(std::move(in), std::move(out))();
            });
        }

}

#endif // INCLUDED__csp__tee_h
