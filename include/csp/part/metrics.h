#pragma once

#include <chrono>
#include <csp/part/part.h>
#include <utility>

namespace csp::part {

struct metrics_snapshot {
    size_t count;
    std::chrono::steady_clock::duration elapsed;
};

// Transparent passthrough reporting throughput stats on a side channel.
// Stats are pull-based: read from the stats reader to get a snapshot.
template <typename T>
std::pair<reader<T>, reader<metrics_snapshot>> metrics(reader<T> data) {
    chan<T> out_ch;
    chan<metrics_snapshot> stats_ch;
    reader<T> data_out = std::move(out_ch.r);
    reader<metrics_snapshot> stats_out = std::move(stats_ch.r);

    csp::spawn([data = std::move(data), out = std::move(out_ch.w),
                stats = std::move(stats_ch.w)]() mutable {
        internal::descr("metrics");

        size_t count = 0;
        auto start = std::chrono::steady_clock::now();
        T t;

        for (;;) {
            auto snap = metrics_snapshot{
                count, std::chrono::steady_clock::now() - start};
            switch (csp::alt(data >> t, stats << snap, ~out)) {
            case 1:  // Data — forward.
                count++;
                if (!(out << std::move(t))) return;
                break;
            case 2:  // Stats pulled — already sent.
                break;
            case ~1:  // Data died — close output, serve remaining stats.
                out = {};
                for (;;) {
                    auto final_snap = metrics_snapshot{
                        count, std::chrono::steady_clock::now() - start};
                    if (!(stats << final_snap)) return;
                }
                return;
            case ~2:  // Stats reader dropped — keep forwarding.
                for (T v; csp::alt(data >> v, ~out) > 0;) {
                    if (!(out << std::move(v))) return;
                }
                return;
            default:  // Out died.
                return;
            }
        }
    });

    return {std::move(data_out), std::move(stats_out)};
}

}
