#pragma once

#include <csp/part/part.h>
#include <csp/timer.h>

#include <chrono>
#include <vector>

namespace csp::part {

// Collect values into frames, emitting each frame when either:
//   - n values have accumulated (batch full), or
//   - the timeout expires (partial frame flushed).
// Like batch, but with a time dimension — ensures data flows even
// when input is slow.  Partial frames on input close are also flushed.
template <typename T>
auto frame(size_t n, duration timeout) {
    return make_filter<T, std::vector<T>>(
        [n, timeout](reader<T> in, writer<std::vector<T>> out) {
            internal::descr("frame");
            std::vector<T> buf;
            buf.reserve(n);
            for (;;) {
                if (buf.empty()) {
                    // Wait for first value (no timer yet).
                    T t;
                    if (csp::alt(in >> t, ~out) != 0) return;
                    buf.push_back(std::move(t));
                    if (buf.size() == n) {
                        if (!(out << std::move(buf))) return;
                        buf.clear();
                        buf.reserve(n);
                    }
                } else {
                    // Have partial frame — race input vs timeout.
                    T t;
                    auto timer = csp::after(timeout);
                    switch (csp::prialt(in >> t, timer >> nullptr, ~out)) {
                    case 0:  // new value
                        buf.push_back(std::move(t));
                        if (buf.size() == n) {
                            if (!(out << std::move(buf))) return;
                            buf.clear();
                            buf.reserve(n);
                        }
                        break;
                    case 1:  // timeout — flush partial frame
                        if (!(out << std::move(buf))) return;
                        buf.clear();
                        buf.reserve(n);
                        break;
                    default: // input or output died
                        if (!buf.empty()) out << std::move(buf);
                        return;
                    }
                }
            }
        });
}

}
