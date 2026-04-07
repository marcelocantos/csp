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
        chanops.push_back({internal::wait_dead(out.internal_writer()), nullptr, internal::get_slot(out.internal_writer().ptr)});
        // Slot 1: read from input (removed when input exhausted).
        chanops.push_back({internal::wait(in.internal_reader()), &new_sub, internal::get_slot(in.internal_reader().ptr)});

        bool input_alive = true;

        while (input_alive || !subs.empty()) {
            size_t sub_base = input_alive ? 2 : 1;

            internal::AltMatch m;
            internal::alt_begin(&m, chanops.data(), static_cast<int>(chanops.size()), 0);

            if (m.src && m.dst) {
                if (input_alive && m.result == 1) {
                    *static_cast<reader<B>*>(m.dst) =
                        std::move(*static_cast<reader<B>*>(m.src));
                } else {
                    *static_cast<B*>(m.dst) =
                        std::move(*static_cast<B*>(m.src));
                }
            }

            internal::alt_end(&m);

            if (m.result == ~0) {
                return;  // Output died.
            } else if (input_alive && m.result == 1) {
                // New sub-reader.
                chanops.push_back({internal::wait(new_sub.internal_reader()), &b, internal::get_slot(new_sub.internal_reader().ptr)});
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
