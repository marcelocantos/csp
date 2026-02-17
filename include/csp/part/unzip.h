#pragma once

#include <csp/part/part.h>

#include <tuple>
#include <type_traits>
#include <utility>

namespace csp::part {

namespace detail {

template <typename Tuple, size_t... Is>
auto make_chans(std::index_sequence<Is...>) {
    return std::make_tuple(csp::chan<std::tuple_element_t<Is, Tuple>>{}...);
}

template <typename Tuple, typename Chans, size_t... Is>
void unzip_write(Tuple& vals, Chans& chans, std::index_sequence<Is...>) {
    // Write each element to its channel. Short-circuit if any write fails.
    (void)((std::get<Is>(chans).w << std::get<Is>(vals)) && ...);
}

template <typename Chans, size_t... Is>
auto extract_readers(Chans& chans, std::index_sequence<Is...>) {
    return std::make_tuple(std::move(std::get<Is>(chans).r)...);
}

} // namespace detail

// Unzip a reader of tuples into N readers, one per tuple element.
// reader<tuple<A, B, ...>> → tuple<reader<A>, reader<B>, ...>
template <typename... Ts>
auto unzip(reader<std::tuple<Ts...>> in) {
    using Tup = std::tuple<Ts...>;
    constexpr auto N = sizeof...(Ts);
    auto seq = std::make_index_sequence<N>{};

    auto chans = detail::make_chans<Tup>(seq);
    auto readers = detail::extract_readers(chans, seq);

    csp::spawn([in = std::move(in), chans = std::move(chans)]() mutable {
        internal::descr("unzip");
        constexpr auto seq = std::make_index_sequence<N>{};
        for (Tup t; in >> t;) {
            detail::unzip_write(t, chans, seq);
        }
    });

    return readers;
}

// Unzip through a decomposing function: f(In) → tuple-like<A, B, ...>.
// reader<In> → tuple<reader<A>, reader<B>, ...>
template <typename In, typename F>
auto unzip(reader<In> in, F&& f) {
    using Ret = std::invoke_result_t<std::decay_t<F>&, In>;
    constexpr auto N = std::tuple_size_v<Ret>;
    auto seq = std::make_index_sequence<N>{};

    auto chans = detail::make_chans<Ret>(seq);
    auto readers = detail::extract_readers(chans, seq);

    csp::spawn([in = std::move(in), f = std::forward<F>(f),
                chans = std::move(chans)]() mutable {
        internal::descr("unzip");
        constexpr auto seq = std::make_index_sequence<N>{};
        for (In v; in >> v;) {
            auto t = f(std::move(v));
            detail::unzip_write(t, chans, seq);
        }
    });

    return readers;
}

}
