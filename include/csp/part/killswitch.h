#pragma once

#include <csp/part/part.h>

namespace csp::part {

    template <typename A>
    auto killswitch(reader<> keepalive) {
        return make_filter<A>([keepalive = std::move(keepalive)](reader<A> in, writer<A> out) {
            internal::descr("killswitch");

            for (A a; prialt(~keepalive, ~out, in >> a) > 0 && prialt(~keepalive, out << a) > 0;) { }
        });
    }

}
