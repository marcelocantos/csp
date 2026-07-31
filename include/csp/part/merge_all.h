#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Flatten a stream of sub-streams by merging all concurrently.
// reader<reader<B>> → reader<B>. Output order is non-deterministic.
// Exits when input is exhausted and all sub-streams are drained,
// or when the output reader is dropped.
template <typename B>
inline auto const merge_all = make_filter<reader<B>, B>([](reader<reader<B>> in, writer<B> out) {
        internal::descr("merge_all");

        reader<B> new_sub;
        B b;
        std::vector<reader<B>> subs;
        std::vector<internal::ChanOp> chanops;

        // Slot 0: death-watch on output.
        chanops.push_back(detail::fan_in_out_dead(out));
        // Slot 1: read from input (removed when input exhausted).
        detail::fan_in_push_read(chanops, in, new_sub);

        bool input_alive = true;

        while (input_alive || !subs.empty()) {
            size_t sub_base = input_alive ? 2 : 1;

            auto m = detail::fan_in_step(chanops, [&](internal::AltMatch& am) {
                if (input_alive && am.result == 1) {
                    *static_cast<reader<B>*>(am.dst) =
                        std::move(*static_cast<reader<B>*>(am.src));
                } else {
                    *static_cast<B*>(am.dst) =
                        std::move(*static_cast<B*>(am.src));
                }
            });

            if (m.result == ~0) {
                return;  // Output died.
            } else if (input_alive && m.result == 1) {
                // New sub-reader.
                detail::fan_in_push_read(chanops, new_sub, b);
                subs.push_back(std::move(new_sub));
            } else if (input_alive && m.result == ~1) {
                // Input exhausted — remove input slot.
                chanops.erase(chanops.begin() + 1);
                input_alive = false;
            } else if (m.result >= 0) {
                // Sub-stream data — forward.
                if (!(out << std::move(b))) return;
            } else {
                // Sub-stream died — swap-and-pop.
                detail::fan_in_remove(subs, chanops,
                                     static_cast<size_t>(~m.result), sub_base);
            }
        }
    });

}
