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
            return (... && (csp::alt(std::get<Is>(readers) >> std::get<Is>(vals), ~out) == 1));
        }

        // Core zipf implementation (f first, fully deducible).
        template <typename F, typename... Ts>
        auto zipf_impl(F&& f, reader<Ts>... rs) {
            using Out = std::invoke_result_t<std::decay_t<F>&, Ts...>;
            return make_producer<Out>(
                [f = std::forward<F>(f), readers = std::make_tuple(std::move(rs)...)]
                (writer<Out> out) mutable {
                    internal::descr("zipf");
                    std::tuple<Ts...> vals;
                    while (zip_read_all(readers, vals, out,
                                        std::index_sequence_for<Ts...>{})) {
                        if (!(out << std::apply(f, std::move(vals)))) return;
                    }
                });
        }

    }

    // Zip N readers through a combining function.
    // Reads one value from each input per cycle, applies f, and emits the result.
    // Terminates when any input or the output dies.
    // Requires explicit type parameters: zipf<int, double>(r1, r2, f).
    template <typename... Ts, typename F>
    auto zipf(reader<Ts>... rs, F&& f) {
        return detail::zipf_impl(std::forward<F>(f), std::move(rs)...);
    }

    // Zip N readers element-wise into tuples.
    template <typename... Ts>
    auto zip(reader<Ts>... rs) {
        return detail::zipf_impl(
            [](Ts... vs) { return std::make_tuple(std::move(vs)...); },
            std::move(rs)...);
    }

    // Zip two readers through a combining function.
    template <typename A, typename B, typename F>
    auto zip2f(reader<A> a, reader<B> b, F&& f) {
        return detail::zipf_impl(std::forward<F>(f), std::move(a), std::move(b));
    }

    // Zip two readers element-wise into pairs.
    template <typename A, typename B>
    auto zip2(reader<A> a, reader<B> b) {
        return detail::zipf_impl(
            [](A a, B b) { return std::make_pair(std::move(a), std::move(b)); },
            std::move(a), std::move(b));
    }

}
