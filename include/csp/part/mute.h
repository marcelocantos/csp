#pragma once

#include <csp/part/part.h>

namespace csp::part {

    // A reader endpoint that never produces values.
    // Useful as a default or placeholder in alt/prialt expressions.
    template <typename T = poke_t>
    auto mute() {
        return make_producer<T>([](writer<T> out) {
            internal::descr("mute");

            alt(~out);
        });
    }

}
