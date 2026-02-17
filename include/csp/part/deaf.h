#pragma once

#include <csp/part/part.h>

namespace csp::part {

    // A writer endpoint that never accepts values.
    // Useful as a default or placeholder in alt/prialt expressions.
    template <typename T>
    auto deaf() {
        return make_consumer<T>([](reader<T> in) {
            internal::descr("deaf");

            static Logger scope("chan/deaf/scope");
            BRAC_SCOPE(scope, "deaf", "");

            alt(~in);
        });
    }

}
