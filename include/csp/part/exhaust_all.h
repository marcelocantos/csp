#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Flatten a stream of sub-streams, ignoring new arrivals while draining.
// reader<reader<B>> → reader<B>. While a sub-stream is active, new sub-streams
// from input are read and discarded. When the current sub-stream dies, the next
// sub-stream from input is accepted.
template <typename B>
inline auto const exhaust_all = make_filter<reader<B>, B>([](reader<reader<B>> in, writer<B> out) {
    internal::descr("exhaust_all");

    reader<B> sub;
    while (csp::alt(in >> sub, ~out) == 0) {
        B b;
        reader<B> discard;
        for (;;) {
            // ~sub vulture fires immediately on sub death (as ~1),
            // ensuring we detect it before the alt matches a ready
            // peer on input (data chanops defer dead-channel).
            switch (csp::prialt(sub >> b, ~sub, in >> discard, ~out)) {
            case 0:  // Sub data — forward.
                if (!(out << std::move(b))) return;
                continue;
            case ~1:  // Sub died (vulture) — outer loop gets next.
                break;
            case 2:  // New sub from input — discard.
                continue;
            case ~2:  // Input died — drain remaining sub.
                for (; csp::alt(sub >> b, ~out) == 0;) {
                    if (!(out << std::move(b))) return;
                }
                return;
            default:  // Output died.
                return;
            }
            break;  // Reached only from case ~1 (sub died).
        }
    }
});

}
