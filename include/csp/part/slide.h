#pragma once

#include <csp/part/part.h>

#include <deque>

namespace csp::part {

// Two-channel window output: elements entering and leaving the window.
template <typename T>
struct window_pair {
    reader<T> in;   // elements entering the window
    reader<T> out;  // elements leaving the window
};

// General sliding window with predicate-based expiry.
// expired(older, current) returns true when older should leave the window.
// Per input element: expire from front (send on out), then send new on in.
// slide_in=true emits during growth; false suppresses until first expiry.
template <typename T, typename Pred>
window_pair<T> slide(reader<T> src, Pred expired, bool slide_in = true) {
    chan<T> in_ch, out_ch;
    window_pair<T> result{std::move(in_ch.r), std::move(out_ch.r)};

    csp::spawn([src = std::move(src), in_w = std::move(in_ch.w),
                out_w = std::move(out_ch.w), expired = std::move(expired),
                slide_in]() mutable {
        internal::descr("slide");

        std::deque<T> win;
        bool started = slide_in;

        for (T t; src >> t;) {
            // Expire stale elements from front.
            while (!win.empty() && expired(win.front(), t)) {
                if (started) {
                    if (!(out_w << std::move(win.front()))) return;
                }
                win.pop_front();
                started = true;
            }

            win.push_back(t);

            if (started) {
                if (!(in_w << std::move(t))) return;
            }
        }
    });

    return result;
}

// Fixed-size sliding window. Expires oldest when window exceeds n elements.
template <typename T>
window_pair<T> slide_fixed(reader<T> src, size_t n, bool slide_in = true) {
    chan<T> in_ch, out_ch;
    window_pair<T> result{std::move(in_ch.r), std::move(out_ch.r)};

    csp::spawn([src = std::move(src), in_w = std::move(in_ch.w),
                out_w = std::move(out_ch.w), n, slide_in]() mutable {
        internal::descr("slide_fixed");

        std::deque<T> win;
        bool started = slide_in;

        for (T t; src >> t;) {
            // Expire oldest if window is full.
            if (win.size() >= n) {
                started = true;
                if (!(out_w << std::move(win.front()))) return;
                win.pop_front();
            }

            win.push_back(t);

            if (started) {
                if (!(in_w << std::move(t))) return;
            }
        }
    });

    return result;
}

}
