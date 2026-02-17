#pragma once

#include <csp/part/part.h>

#include <csp/internal/on_scope_exit.h>

namespace csp::part {

    // Duplicate a stream to a side channel.
    // Each value is written to both the main output and the side writer.
    // If the side channel dies, values continue flowing to the main output.
    template <typename T>
    auto tee(writer<T> side) {
        return make_filter<T>([side = std::move(side)](reader<T> in, writer<T> out) {
            internal::descr("tee");

            static Logger scope("chan/tee/scope");
            BRAC_SCOPE(scope, "tee", "");

            static Logger log("chan/tee/log");

            for (T t; prialt(~out, in >> t) > 0 && out << t && side << std::move(t);) { }
            for (T t; prialt(~out, in >> t) > 0 && out << std::move(t);) { }
        });
    }

}
