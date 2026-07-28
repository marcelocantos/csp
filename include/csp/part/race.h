#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Race N input readers: emit values from whichever source produces
// first.  All sources are multiplexed via dynamic prialt.  When a
// source dies it's removed from the race.  Output closes when all
// sources are dead or the output reader dies.
//
// Unlike merge (which interleaves all sources fairly), race is
// biased toward the fastest source — slow sources may starve.
//
// Eager by design: race returns live endpoints (a reader<T> backed by
// an already-spawned imp), not a lazy make_* part.  The lazy convention
// (see part.h) does not apply — converting would be a user-visible
// breaking change.
template <typename T>
reader<T> race(std::vector<reader<T>> sources) {
    return spawn_producer<T>(
        [sources = std::move(sources)](writer<T> out) mutable {
            internal::descr("race");
            while (!sources.empty()) {
                // Build dynamic prialt ops: one read per source + output death.
                std::vector<chan_op<T>> ops;
                ops.reserve(sources.size() + 1);
                T val;
                for (auto& s : sources) {
                    ops.push_back(s >> val);
                }
                ops.push_back(~out);

                int k = csp::prialt(ops);

                if (k == ~static_cast<int>(sources.size())) {
                    // Output died.
                    return;
                }

                if (k >= 0) {
                    // Got a value from source k.
                    if (!(out << std::move(val))) return;
                } else {
                    // Source ~k died — remove it.
                    int dead = ~k;
                    sources.erase(sources.begin() + dead);
                }
            }
        });
}

}
