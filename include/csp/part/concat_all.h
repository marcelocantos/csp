#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Flatten a stream of sub-streams by draining each sequentially.
// reader<reader<B>> → reader<B>. Output preserves sub-stream order.
// Each sub-stream is fully consumed before the next is read from input.
template <typename B>
auto concat_all() {
    return make_filter<reader<B>, B>([](reader<reader<B>> in, writer<B> out) {
        internal::descr("concat_all");

        for (reader<B> sub; csp::alt(in >> sub, ~out) == 0;) {
            for (B b; csp::alt(sub >> b, ~out) == 0;) {
                if (!(out << std::move(b))) return;
            }
        }
    });
}

}
