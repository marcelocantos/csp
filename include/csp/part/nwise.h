#pragma once

#include <csp/part/part.h>

#include <array>
#include <tuple>
#include <utility>

namespace csp::part {

namespace detail {

template <typename T, size_t N, size_t... Is>
auto array_to_tuple(std::array<T, N>& arr, std::index_sequence<Is...>) {
    return std::make_tuple(arr[Is]...);
}

} // namespace detail

// Sliding N-element window emitting tuples.
// nwise<N, T>() → filter: reader<T> → reader<tuple<T, T, ..., T>>
// Input of fewer than N elements produces no output.
template <size_t N, typename T>
auto nwise() {
    static_assert(N >= 2, "nwise requires N >= 2");

    // Build the output tuple type: tuple<T, T, ..., T> (N copies).
    using Out = decltype(detail::array_to_tuple(
        std::declval<std::array<T, N>&>(), std::make_index_sequence<N>{}));

    return make_filter<T, Out>(
        [](reader<T> in, writer<Out> out) {
            internal::descr("nwise");
            std::array<T, N> buf;

            // Fill the initial window.
            for (size_t i = 0; i < N; ++i) {
                if (csp::alt(in >> buf[i], ~out) != 1) return;
            }
            if (!(out << detail::array_to_tuple(buf, std::make_index_sequence<N>{})))
                return;

            // Slide: shift left, read into last slot.
            for (;;) {
                for (size_t i = 0; i + 1 < N; ++i)
                    buf[i] = std::move(buf[i + 1]);
                if (csp::alt(in >> buf[N - 1], ~out) != 1) return;
                if (!(out << detail::array_to_tuple(buf, std::make_index_sequence<N>{})))
                    return;
            }
        });
}

}
