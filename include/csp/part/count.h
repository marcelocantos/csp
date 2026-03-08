#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Generate [start, start+step, ...) up to stop.
template <typename T>
auto count(T start, T stop, T step = 1, bool cyclic = false) {
    return make_producer<T>([start, stop, step, cyclic](writer<T> sink) {
        internal::descr("count");

        static Logger s_log("chan/count");
        BRAC_SCOPE(s_log, "count", "..., cyclic=%s", cyclic ? "true" : "false");

        T i = start;
        do {
            for (; i < stop; i += step) {
                if (!(sink << i)) {
                    return;
                }
            }
            i -= stop - start;
        } while (cyclic);
    });
}

// Generate [start, start+step, ...) indefinitely.
template <typename T>
auto count_forever(T start, T step = 1) {
    return make_producer<T>([start, step](writer<T> sink) {
        internal::descr("count_∞");

        static Logger s_log("chan/count_forever");
        BRAC_SCOPE(s_log, "count_forever", "");

        for (T i = start; sink << i; i += step) { }
    });
}

}
