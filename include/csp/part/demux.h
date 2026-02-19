#pragma once

#include <csp/part/part.h>
#include <csp/part/unzip.h>

#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace csp::part {

namespace detail {

template <typename T, typename V, size_t I = 0>
constexpr size_t variant_index() {
    static_assert(I < std::variant_size_v<V>, "T not found in variant");
    if constexpr (std::is_same_v<T, std::variant_alternative_t<I, V>>)
        return I;
    else
        return variant_index<T, V, I + 1>();
}

template <typename T, typename V>
inline constexpr size_t variant_index_v = variant_index<T, V>();

} // namespace detail

// Split a variant stream into N typed readers, one per alternative.
// reader<variant<A, B, ...>> → tuple<reader<A>, reader<B>, ...>
template <typename... Ts>
auto demux(reader<std::variant<Ts...>> in) {
    using V = std::variant<Ts...>;
    constexpr auto N = sizeof...(Ts);
    auto seq = std::make_index_sequence<N>{};

    auto chans = detail::make_chans<std::tuple<Ts...>>(seq);
    auto readers = detail::extract_readers(chans, seq);

    csp::spawn([in = std::move(in), chans = std::move(chans)]() mutable {
        internal::descr("demux");
        for (V v; in >> v;) {
            std::visit([&chans](auto&& val) {
                using T = std::decay_t<decltype(val)>;
                constexpr size_t I = detail::variant_index_v<T, V>;
                std::get<I>(chans).w << std::move(val);
            }, std::move(v));
        }
    });

    return readers;
}

}
