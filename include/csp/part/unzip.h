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

// Unzip through a decomposing function: f(In) → tuple<A, B, ...>.
// reader<In> → tuple<reader<A>, reader<B>, ...>
template <typename In, typename F>
auto unzipf(reader<In> in, F&& f) {
    using Tup = std::invoke_result_t<std::decay_t<F>&, In>;
    constexpr auto N = std::tuple_size_v<Tup>;
    auto seq = std::make_index_sequence<N>{};

    auto chans = detail::make_chans<Tup>(seq);
    auto readers = detail::extract_readers(chans, seq);

    csp::spawn([in = std::move(in), f = std::forward<F>(f),
                chans = std::move(chans)]() mutable {
        internal::descr("unzipf");
        constexpr auto seq = std::make_index_sequence<N>{};
        for (In v; in >> v;) {
            auto t = f(std::move(v));
            detail::unzip_write(t, chans, seq);
        }
    });

    return readers;
}

// Unzip a reader of pairs into two readers.
// reader<pair<A, B>> → pair<reader<A>, reader<B>>
template <typename A, typename B>
auto unzip2(reader<std::pair<A, B>> in) {
    auto ca = csp::chan<A>{};
    auto cb = csp::chan<B>{};
    auto ra = std::move(ca.r);
    auto rb = std::move(cb.r);

    csp::spawn([in = std::move(in), wa = std::move(ca.w),
                wb = std::move(cb.w)]() mutable {
        internal::descr("unzip2");
        for (std::pair<A, B> p; in >> p;) {
            wa << std::move(p.first);
            wb << std::move(p.second);
        }
    });

    return std::make_pair(std::move(ra), std::move(rb));
}

// Unzip through a two-output decomposing function.
// f(In) → pair<A, B>; reader<In> → pair<reader<A>, reader<B>>
template <typename In, typename F>
auto unzip2f(reader<In> in, F&& f) {
    using P = std::invoke_result_t<std::decay_t<F>&, In>;
    using A = typename P::first_type;
    using B = typename P::second_type;

    auto ca = csp::chan<A>{};
    auto cb = csp::chan<B>{};
    auto ra = std::move(ca.r);
    auto rb = std::move(cb.r);

    csp::spawn([in = std::move(in), f = std::forward<F>(f),
                wa = std::move(ca.w), wb = std::move(cb.w)]() mutable {
        internal::descr("unzip2f");
        for (In v; in >> v;) {
            auto p = f(std::move(v));
            wa << std::move(p.first);
            wb << std::move(p.second);
        }
    });

    return std::make_pair(std::move(ra), std::move(rb));
}

}
