#pragma once

#include <csp/part/part.h>
#include "mute.h"

namespace csp::part {

    template <typename T>
    auto latch() {
        return make_filter<T>([](reader<T> in, writer<T> out) {
            internal::descr("latch");

            T t;
            if (prialt(~out, in >> t) > 0) {
                while (prialt(in >> t, out << t) > 0) { }
                while (out << t) { }
            }
        });
    }

}
