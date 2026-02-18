#pragma once

#include <csp/part/part.h>
#include <csp/ringbuffer.h>

namespace csp::part {

// Emit the first n elements, then close.
template <typename T>
auto first(size_t n) {
    return make_filter<T>([n](reader<T> in, writer<T> out) {
        internal::descr("first");
        for (size_t i = 0; i < n; ++i) {
            T t;
            if (csp::alt(in >> t, ~out) != 0) return;
            if (!(out << std::move(t))) return;
        }
    });
}

// Emit the last n elements (buffered until input closes).
template <typename T>
auto last(size_t n) {
    return make_filter<T>([n](reader<T> in, writer<T> out) {
        internal::descr("last");
        if (n == 0) {
            for (T t; csp::alt(in >> t, ~out) == 0;) {}
            return;
        }
        csp::detail::RingBuffer<T> buf(n);
        for (T t; csp::alt(in >> t, ~out) == 0;) {
            if (buf.full()) buf.pop();
            buf.push(std::move(t));
        }
        for (auto& v : buf) {
            if (!(out << std::move(v))) return;
        }
    });
}

// Drop the first n elements, then pass through the rest.
template <typename T>
auto skip_first(size_t n) {
    return make_filter<T>([n](reader<T> in, writer<T> out) {
        internal::descr("skip_first");
        for (size_t i = 0; i < n; ++i) {
            T t;
            if (csp::alt(in >> t, ~out) != 0) return;
        }
        for (T t; csp::alt(in >> t, ~out) == 0;) {
            if (!(out << std::move(t))) return;
        }
    });
}

// Emit all but the last n elements (delayed by n; remainder discarded).
template <typename T>
auto skip_last(size_t n) {
    return make_filter<T>([n](reader<T> in, writer<T> out) {
        internal::descr("skip_last");
        if (n == 0) {
            for (T t; csp::alt(in >> t, ~out) == 0;) {
                if (!(out << std::move(t))) return;
            }
            return;
        }
        csp::detail::RingBuffer<T> buf(n);
        for (T t; csp::alt(in >> t, ~out) == 0;) {
            if (buf.full()) {
                if (!(out << std::move(buf.front()))) return;
                buf.pop();
            }
            buf.push(std::move(t));
        }
    });
}

}
