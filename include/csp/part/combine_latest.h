#pragma once

#include <csp/part/part.h>

#include <array>
#include <tuple>
#include <type_traits>
#include <utility>

namespace csp::part {

namespace detail {

// Set up ChanOp read slots for combine_latest inputs.
template <size_t... Is, typename Bufs, typename Inputs>
void combine_setup(internal::ChanOp* chanops, Bufs& bufs, Inputs& inputs,
                   std::index_sequence<Is...>) {
    ((chanops[Is] = {internal::wait(std::get<Is>(inputs).internal_reader()),
                     &std::get<Is>(bufs),
                     internal::get_slot(std::get<Is>(inputs).internal_reader().ptr)}), ...);
}

// Dispatch tables for typed transfer and latest-update by runtime index.
template <typename... Ts>
struct combine_dispatch {
    using xfer_fn = void(*)(void*, void*);
    using update_fn = void(*)(std::tuple<Ts...>&, std::tuple<Ts...>&);

    template <size_t I>
    static void transfer(void* d, void* s) {
        using T = std::tuple_element_t<I, std::tuple<Ts...>>;
        *static_cast<T*>(d) = std::move(*static_cast<T*>(s));
    }

    template <size_t I>
    static void update(std::tuple<Ts...>& latest, std::tuple<Ts...>& bufs) {
        std::get<I>(latest) = std::move(std::get<I>(bufs));
    }

    template <size_t... Is>
    static constexpr auto make_transfers(std::index_sequence<Is...>) {
        return std::array<xfer_fn, sizeof...(Is)>{{&transfer<Is>...}};
    }

    template <size_t... Is>
    static constexpr auto make_updaters(std::index_sequence<Is...>) {
        return std::array<update_fn, sizeof...(Is)>{{&update<Is>...}};
    }

    static constexpr auto transfers =
        make_transfers(std::index_sequence_for<Ts...>{});
    static constexpr auto updaters =
        make_updaters(std::index_sequence_for<Ts...>{});
};

// Core combine_latest loop parameterised on an emit function.
template <typename F, typename... Ts>
auto combine_latest_impl(F&& f, reader<Ts>... inputs) {
    using Out = std::invoke_result_t<std::decay_t<F>&, Ts&...>;
    constexpr size_t N = sizeof...(Ts);
    using Dispatch = combine_dispatch<Ts...>;

    return make_producer<Out>(
        [f = std::forward<F>(f),
         inputs = std::tuple{std::move(inputs)...}](writer<Out> out) mutable {
            internal::descr("combine_latest");

            std::tuple<Ts...> bufs;
            std::tuple<Ts...> latest;
            std::array<bool, N> has{};
            size_t count = 0;
            size_t alive = N;

            internal::ChanOp chanops[N + 1];
            // Slot 0: death-watch on output.
            chanops[0] = {internal::wait_dead(out.internal_writer()), nullptr, internal::get_slot(out.internal_writer().ptr)};
            // Slots 1..N: reads from each input.
            combine_setup(chanops + 1, bufs, inputs,
                          std::index_sequence_for<Ts...>{});

            while (alive > 0) {
                internal::AltMatch m;
                internal::alt_begin(&m, chanops, N + 1, 0);
                if (m.src && m.dst) {
                    int idx = (m.result >= 0 ? m.result : ~m.result) - 1;
                    Dispatch::transfers[idx](m.dst, m.src);
                }
                internal::alt_end(&m);

                if (m.result == ~0) {
                    return; // Output died.
                } else if (m.result > 0) {
                    size_t i = static_cast<size_t>(m.result) - 1;
                    Dispatch::updaters[i](latest, bufs);
                    if (!has[i]) { has[i] = true; ++count; }
                    if (count == N) {
                        if (!(out << std::apply(f, latest))) return;
                    }
                } else if (m.result < 0) {
                    // Input died.
                    size_t slot = static_cast<size_t>(~m.result);
                    size_t i = slot - 1;
                    if (!has[i]) return; // Never produced — can't complete.
                    chanops[slot] = {{}, nullptr};
                    --alive;
                }
            }
        });
}

} // namespace detail

// Emit a tuple of latest values whenever any input updates.
// No output until all inputs have produced at least one value.
// When an input closes, its last value is retained; output closes when
// all inputs close or output dies.  If any input closes before ever
// producing a value, output closes immediately.
template <typename... Ts>
auto combine_latest(reader<Ts>... rs) {
    static_assert(sizeof...(Ts) >= 2, "combine_latest requires at least 2 inputs");
    return detail::combine_latest_impl(
        [](Ts&... vs) { return std::tuple{vs...}; },
        std::move(rs)...);
}

// combine_latest with combining function (requires explicit type params).
template <typename... Ts, typename F>
    requires std::is_invocable_v<std::decay_t<F>&, Ts&...>
auto combine_latest(reader<Ts>... rs, F&& f) {
    static_assert(sizeof...(Ts) >= 2, "combine_latest requires at least 2 inputs");
    return detail::combine_latest_impl(std::forward<F>(f), std::move(rs)...);
}

}
