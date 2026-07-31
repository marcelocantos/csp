#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Maps each input element to a sub-stream via f, then merges all sub-streams
// into one output. Sub-streams are read concurrently (non-deterministic order).
// Output closes when the input is exhausted and all sub-streams are drained.
//
// Kept as a single-imp specialised body rather than `map | merge_all`
// (🎯T51): composition adds an intermediate channel + map imp per pipeline
// (extra rendezvous hop on every outer element) and delays cancellation
// propagation by one hop on output death. See docs/design/fan-in-unification.md.
template <typename A, typename B, typename F>
auto flat_map(F&& f) {
    return make_filter<A, B>([f = std::forward<F>(f)](reader<A> in, writer<B> out) {
        internal::descr("flat_map");

        A a;
        B b;
        std::vector<reader<B>> subs;
        std::vector<internal::ChanOp> chanops;

        // Slot 0: death-watch on output.
        chanops.push_back(detail::fan_in_out_dead(out));
        // Slot 1: read from input (removed when input exhausted).
        detail::fan_in_push_read(chanops, in, a);

        bool input_alive = true;

        while (input_alive || !subs.empty()) {
            size_t sub_base = input_alive ? 2 : 1;

            auto m = detail::fan_in_step(chanops, [&](internal::AltMatch& am) {
                // Type-aware transfer: input slot reads A, sub-stream slots read B.
                if (input_alive && am.result == 1) {
                    *static_cast<A*>(am.dst) = std::move(*static_cast<A*>(am.src));
                } else {
                    *static_cast<B*>(am.dst) = std::move(*static_cast<B*>(am.src));
                }
            });

            if (m.result == ~0) {
                // Output died.
                return;
            } else if (input_alive && m.result == 1) {
                // New input element — spawn sub-stream.
                reader<B> sub = f(std::move(a));
                detail::fan_in_push_read(chanops, sub, b);
                subs.push_back(std::move(sub));
            } else if (input_alive && m.result == ~1) {
                // Input exhausted — remove input slot.
                chanops.erase(chanops.begin() + 1);
                input_alive = false;
            } else if (m.result >= 0) {
                // Sub-stream data — forward to output.
                if (!(out << std::move(b))) return;
            } else {
                // Sub-stream died — swap-and-pop.
                detail::fan_in_remove(subs, chanops,
                                     static_cast<size_t>(~m.result), sub_base);
            }
        }
    });
}

}
