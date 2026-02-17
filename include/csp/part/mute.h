#pragma once

#include <csp/part/part.h>

namespace csp::part {

    template <typename T = poke_t>
    auto mute() {
        return make_producer<T>([](writer<T> out) {
            internal::descr("mute");

            alt(~out);
        });
    }

}
