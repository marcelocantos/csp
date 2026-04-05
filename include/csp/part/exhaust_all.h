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
            // ~sub is at slot 0 so it fires immediately in Phase 1
            // when sub is already dead. In Phase 2 (sub dies while
            // sleeping) either ~sub (~0) or sub >> b (~1) may win the
            // CAS — both indicate sub death. ~in (~2) is impossible here
            // since only writers can die and trigger dead-data; ~out (~3)
            // means output died.
            switch (csp::prialt(~sub, sub >> b, in >> discard, ~out)) {
            case ~0:  // Sub died (vulture won Phase 1 or Phase 2 CAS).
            case ~1:  // Sub died (data chanop won Phase 2 CAS).
                break;
            case 1:  // Sub data — forward.
                if (!(out << std::move(b))) return;
                continue;
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
            break;  // Reached only from case ~0 or ~1 (sub died).
        }
    }
});

}
