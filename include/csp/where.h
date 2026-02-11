#ifndef INCLUDED__csp__where_h
#define INCLUDED__csp__where_h

#include <csp/microthread.h>

namespace csp {

    namespace chan {

        template <typename T, typename Pred>
        auto where(reader<T> in, writer<T> out, Pred && pred) {
            return [in = std::move(in), out = std::move(out), pred]{
                csp_descr("chan::where");

                static Logger log("chan/where");
                CSP_LOG(log, "start");

                for (T t; csp::alt(in >> t, ~out) == 1;) {
                    CSP_LOG(log, "loop");
                    if (pred(t) && !(out << t)) {
                        break;
                    }
                }
                CSP_LOG(log, "finish");
            };
        }

        // Wire up an existing downstream writer, returning an upstream writer.
        template <typename T, typename Pred>
        writer<T> spawn_where(writer<T> w, Pred && pred) {
            return spawn_consumer<T>([w = std::move(w), pred = std::move(pred)](auto && r) {
                where(r, w, pred)();
            });
        }

        // Wire up an existing upstream reader, returning a downstream reader.
        template <typename T, typename Pred>
        reader<T> spawn_where(reader<T> r, Pred && pred) {
            return spawn_producer<T>([=, r = std::move(r), pred = std::move(pred)](auto && w) {
                where(r, w, pred)();
            });
        }

        template <typename T, typename Pred>
        channel<T> spawn_where(Pred && pred) {
            return spawn_filter<T>([=](auto && r, auto && w) {
                where(r, w, pred)();
            });
        }

    }

}

#endif // INCLUDED__csp__where_h
