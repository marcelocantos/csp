#pragma once

#include <csp/part/part.h>

#include <tuple>
#include <utility>
#include <variant>

namespace csp::part {

namespace detail {

// Set up ChanOp read slots and buffer pointer array for mux.
template <size_t... Is, typename Bufs, typename Inputs>
void mux_setup(internal::ChanOp* chanops, void** buf_ptrs,
               Bufs& bufs, Inputs& inputs,
               std::index_sequence<Is...>) {
    ((chanops[Is] = {internal::wait(std::get<Is>(inputs).internal_reader()),
                     &std::get<Is>(bufs),
                     internal::get_slot(std::get<Is>(inputs).internal_reader().ptr)}), ...);
    ((buf_ptrs[Is] = &std::get<Is>(bufs)), ...);
}

// Transfer and wrapper dispatch tables, built at compile time via
// index_sequence expansion.
template <typename V, typename... Ts>
struct mux_dispatch {
    using xfer_fn = void(*)(void*, void*);
    using wrap_fn = V(*)(void*);

    template <size_t I>
    static void transfer(void* d, void* s) {
        using T = std::tuple_element_t<I, std::tuple<Ts...>>;
        *static_cast<T*>(d) = std::move(*static_cast<T*>(s));
    }

    template <size_t I>
    static V wrap(void* p) {
        using T = std::tuple_element_t<I, std::tuple<Ts...>>;
        return V{std::in_place_index<I>, std::move(*static_cast<T*>(p))};
    }

    template <size_t... Is>
    static constexpr auto make_transfers(std::index_sequence<Is...>) {
        return std::array<xfer_fn, sizeof...(Is)>{{&transfer<Is>...}};
    }

    template <size_t... Is>
    static constexpr auto make_wrappers(std::index_sequence<Is...>) {
        return std::array<wrap_fn, sizeof...(Is)>{{&wrap<Is>...}};
    }

    static constexpr auto transfers =
        make_transfers(std::index_sequence_for<Ts...>{});
    static constexpr auto wrappers =
        make_wrappers(std::index_sequence_for<Ts...>{});
};

} // namespace detail

// Non-deterministic merge of N heterogeneous readers into a variant stream.
// Reads from whichever input is ready first. When an input dies it is
// disabled; output closes when all inputs are exhausted or output dies.
template <typename... Ts>
auto mux(reader<Ts>... inputs) {
    using V = std::variant<Ts...>;
    constexpr size_t N = sizeof...(Ts);
    using Dispatch = detail::mux_dispatch<V, Ts...>;

    return make_producer<V>(
        [inputs = std::tuple{std::move(inputs)...}](writer<V> out) mutable {
            internal::descr("mux");

            std::tuple<Ts...> bufs;
            internal::ChanOp chanops[N + 1];
            void* buf_ptrs[N];

            // Slot 0: death-watch on output.
            chanops[0] = {internal::wait_dead(out.internal_writer()), nullptr, internal::get_slot(out.internal_writer().ptr)};
            // Slots 1..N: reads, each pointing at its typed buffer.
            detail::mux_setup(
                chanops + 1, buf_ptrs, bufs, inputs,
                std::index_sequence_for<Ts...>{});

            size_t live = N;
            while (live > 0) {
                internal::AltMatch m;
                internal::alt_begin(&m, chanops, N + 1, 0);
                if (m.src && m.dst) {
                    int idx = (m.result >= 0 ? m.result : ~m.result) - 1;
                    Dispatch::transfers[idx](m.dst, m.src);
                }
                internal::alt_end(&m);

                if (m.result == ~0) {
                    return; // output died
                } else if (m.result > 0) {
                    size_t i = static_cast<size_t>(m.result) - 1;
                    if (!(out << Dispatch::wrappers[i](buf_ptrs[i]))) return;
                } else if (m.result < 0) {
                    // Input died.
                    size_t slot = static_cast<size_t>(~m.result);
                    chanops[slot] = {{}, nullptr};
                    --live;
                }
            }
        });
}

}
