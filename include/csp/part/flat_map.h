#pragma once

#include <csp/part/part.h>

#include <vector>

namespace csp::part {

// Maps each input element to a sub-stream via f, then merges all sub-streams
// into one output. Sub-streams are read concurrently (non-deterministic order).
// Output closes when the input is exhausted and all sub-streams are drained.
template <typename A, typename B, typename F>
auto flat_map(F&& f) {
    return make_filter<A, B>([f = std::forward<F>(f)](reader<A> in, writer<B> out) {
        internal::descr("flat_map");

        A a;
        B b;
        std::vector<reader<B>> subs;
        std::vector<internal::ChanOp> chanops;

        // Slot 0: death-watch on output.
        chanops.push_back({internal::wait_dead(out.internal_writer()), nullptr});
        // Slot 1: read from input (removed when input exhausted).
        chanops.push_back({internal::wait(in.internal_reader()), &a});

        bool input_alive = true;

        while (input_alive || !subs.empty()) {
            size_t sub_base = input_alive ? 2 : 1;

            internal::AltMatch m;
            internal::alt_begin(&m, chanops.data(), chanops.size(), 0);

            // Type-aware transfer: input slot reads A, sub-stream slots read B.
            if (m.src && m.dst) {
                if (input_alive && m.result == 1) {
                    *static_cast<A*>(m.dst) = std::move(*static_cast<A*>(m.src));
                } else {
                    *static_cast<B*>(m.dst) = std::move(*static_cast<B*>(m.src));
                }
            }

            internal::alt_end(&m);

            if (m.result == ~0) {
                // Output died.
                return;
            } else if (input_alive && m.result == 1) {
                // New input element — spawn sub-stream.
                reader<B> sub = f(std::move(a));
                chanops.push_back({internal::wait(sub.internal_reader()), &b});
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
                size_t slot = static_cast<size_t>(~m.result);
                size_t i = slot - sub_base;
                subs[i] = std::move(subs.back());
                subs.pop_back();
                chanops[slot] = chanops.back();
                chanops.pop_back();
            }
        }
    });
}

}
