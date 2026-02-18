#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Forward values until a keepalive channel dies.
// When the keepalive reader closes, the filter shuts down immediately.
template <typename A>
auto killswitch(reader<> keepalive) {
    return make_filter<A>([keepalive = std::move(keepalive)](reader<A> in, writer<A> out) {
        internal::descr("killswitch");

        for (A a; prialt(~keepalive, ~out, in >> a) >= 0 && prialt(~keepalive, out << std::move(a)) >= 0;) { }
    });
}

}
