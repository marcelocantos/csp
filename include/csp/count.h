#pragma once

#include <csp/csp.h>

namespace csp {

    // Emit values according to start/stop/step.  Iff cyclic, wrap around and
    // continue, keeping any residue from overflowing stop.  Confused?  Imagine
    // stepping forever around a ribbon numbered from start to stop - 1.
    //
    // Why not start each cycle at start?  Because that's a special case in
    // which step divides stop - start.
    template <typename T>
    auto count(writer<T> sink, T start, T stop, T step = 1, bool cyclic = false) {
        return [sink = std::move(sink), start, stop, step, cyclic]{
            csp_descr("count");

            static Logger log("chan/count");
            BRAC_SCOPE(log, "count", "..., cyclic=%s", cyclic ? "true" : "false");

            T i = start;
            do {
                for (; i < stop; i += step) {
                    if (!(sink << i)) {
                        return;
                    }
                }
                i -= stop - start;
            } while (cyclic);
        };
    }

    template <typename T>
    auto count_forever(writer<T> sink, T start, T step = 1) {
        return [sink = std::move(sink), start, step]{
            csp_descr("count_∞");

            static Logger log("chan/count_forever");
            BRAC_SCOPE(log, "count_forever", "");

            for (T i = start; sink << i; i += step) { }
        };
    }

    template <typename T>
    reader<T> spawn_count(T start, T stop, T step = 1, bool cyclic = false) {
        return spawn_producer<T>([=](auto w) {
            count(std::move(w), start, stop, step, cyclic)();
        });
    }

    template <typename T>
    reader<T> spawn_count_forever(T start, T step = 1) {
        return spawn_producer<T>([=](auto w) {
            count_forever(std::move(w), start, step)();
        });
    }

}
