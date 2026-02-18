#pragma once

#include <csp/part/part.h>
#include <csp/timer.h>

#include <deque>
#include <utility>

namespace csp::part {

// Delay each value by duration d. Values are queued with absolute deadlines
// and emitted in order. Multiple in-flight values are delayed independently
// (not serialized). On input close, remaining values are drained with their
// original delays.
template <typename T>
auto delay(csp::clock::duration d) {
    return make_filter<T>([d](reader<T> in, writer<T> out) {
        internal::descr("delay");
        std::deque<std::pair<T, csp::clock::time_point>> q;

        for (;;) {
            if (q.empty()) {
                // Nothing pending — wait for input.
                T t;
                if (csp::alt(in >> t, ~out) != 1) return;
                q.emplace_back(std::move(t), csp::clock::now() + d);
            } else {
                // Emit any items already past their deadline.
                while (!q.empty() && q.front().second <= csp::clock::now()) {
                    if (!(out << std::move(q.front().first))) return;
                    q.pop_front();
                }
                if (q.empty()) continue;

                // Wait for input or oldest item's deadline.
                auto timer = csp::after(q.front().second - csp::clock::now());
                T t;
                poke_t p;
                switch (csp::alt(in >> t, timer >> p, ~out)) {
                case 1:  // New value — enqueue.
                    q.emplace_back(std::move(t), csp::clock::now() + d);
                    break;
                case 2:  // Timer fired — emit front.
                    if (!(out << std::move(q.front().first))) return;
                    q.pop_front();
                    break;
                case ~1:  // Input died — drain remaining with delays.
                    for (auto& [val, dl] : q) {
                        csp::sleep_until(dl);
                        if (!(out << std::move(val))) return;
                    }
                    return;
                default:
                    return;
                }
            }
        }
    });
}

}
