#pragma once

#include <csp/part/part.h>

#include <tuple>
#include <type_traits>
#include <utility>

namespace csp::part {

namespace detail {

template <typename Readers, typename Vals, typename Out, size_t... Is>
bool zip_read_all(Readers& readers, Vals& vals,
                  writer<Out> const& out, std::index_sequence<Is...>) {
    return (... && (csp::alt(std::get<Is>(readers) >> std::get<Is>(vals), ~out) == 0));
}

// Core zip implementation with combining function (f first, fully deducible).
template <typename F, typename... Ts>
auto zip_impl(F&& f, reader<Ts>... rs) {
    using Out = std::invoke_result_t<std::decay_t<F>&, Ts...>;
    return make_producer<Out>(
        [f = std::forward<F>(f), readers = std::tuple{std::move(rs)...}]
        (writer<Out> out) mutable {
            internal::descr("zip");
            std::tuple<Ts...> vals;
            while (zip_read_all(readers, vals, out,
                                std::index_sequence_for<Ts...>{})) {
                if (!(out << std::apply(f, std::move(vals)))) return;
            }
        });
}

} // namespace detail

// Zip N readers through a combining function.
// Requires explicit type parameters: zip<int, double>(r1, r2, f).
template <typename... Ts, typename F>
    requires std::is_invocable_v<std::decay_t<F>&, Ts...>
auto zip(reader<Ts>... rs, F&& f) {
    return detail::zip_impl(std::forward<F>(f), std::move(rs)...);
}

// Zip N readers element-wise into tuples.
template <typename... Ts>
auto zip(reader<Ts>... rs) {
    return detail::zip_impl(
        [](Ts... vs) { return std::tuple{std::move(vs)...}; },
        std::move(rs)...);
}

}
