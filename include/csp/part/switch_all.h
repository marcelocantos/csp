#pragma once

#include <csp/part/part.h>

namespace csp::part {

// Flatten a stream of sub-streams, switching to the latest.
// reader<reader<B>> → reader<B>. When a new sub-stream arrives, the previous
// one is cancelled (reader dropped). Only the most recent sub-stream is active.
template <typename B>
inline auto const switch_all = make_filter<reader<B>, B>([](reader<reader<B>> in, writer<B> out) {
    internal::descr("switch_all");

    reader<B> sub;
    if (csp::alt(in >> sub, ~out) != 0) return;

    for (;;) {
        B b;
        reader<B> new_sub;
        switch (csp::alt(sub >> b, in >> new_sub, ~out)) {
        case 0:  // Sub data — forward.
            if (!(out << std::move(b))) return;
            break;
        case 1:  // New sub — switch (old sub dropped = cancelled).
            sub = std::move(new_sub);
            break;
        case ~0:  // Sub died — wait for next.
            if (csp::alt(in >> sub, ~out) != 0) return;
            break;
        case ~1:  // Input died — drain remaining sub.
            for (B val; csp::alt(sub >> val, ~out) == 0;) {
                if (!(out << std::move(val))) return;
            }
            return;
        default:  // Output died.
            return;
        }
    }
});

}
