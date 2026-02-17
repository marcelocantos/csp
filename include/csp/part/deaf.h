#pragma once

#include <csp/part/part.h>

namespace csp::part {

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
