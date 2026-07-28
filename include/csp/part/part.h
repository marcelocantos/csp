#pragma once

#include <csp/csp.h>

#include <array>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace csp::part {

// --- Part-author conventions ---
//
// First-element state: prime, don't flag.  When a part needs "the first
// element is special" state, prime with one leading alt read against
// ~out before entering the steady loop (see diff, pairwise, distinct)
// instead of carrying a bool flag through every iteration.  Keep a flag
// only when *absence* must be tracked across a multi-arm alt — i.e. the
// part can act before any value has arrived and must know whether one
// has (sample, share, slide).

// Wrapper for a reader-consuming combinator body.
// spawn() creates a channel and imp; bind() returns a deferred
// callable; operator() runs inline.
template <typename T, typename F>
struct consumer {
    F body_;

    void operator()(reader<T> r) { body_(std::move(r)); }

    auto bind(reader<T> r) const & {
        return [b = body_, r = std::move(r)]() mutable {
            b(std::move(r));
        };
    }
    auto bind(reader<T> r) && {
        return [b = std::move(body_), r = std::move(r)]() mutable {
            b(std::move(r));
        };
    }

    writer<T> spawn() const & {
        return spawn_consumer<T>(
            [b = body_](reader<T> r) mutable {
                b(std::move(r));
            });
    }
    writer<T> spawn() && {
        return spawn_consumer<T>(
            [b = std::move(body_)](reader<T> r) mutable {
                b(std::move(r));
            });
    }
};

// Wrapper for a writer-producing combinator body.
template <typename T, typename F>
struct producer {
    F body_;

    void operator()(writer<T> w) { body_(std::move(w)); }

    auto bind(writer<T> w) const & {
        return [b = body_, w = std::move(w)]() mutable {
            b(std::move(w));
        };
    }
    auto bind(writer<T> w) && {
        return [b = std::move(body_), w = std::move(w)]() mutable {
            b(std::move(w));
        };
    }

    reader<T> spawn() const & {
        return spawn_producer<T>(
            [b = body_](writer<T> w) mutable {
                b(std::move(w));
            });
    }
    reader<T> spawn() && {
        return spawn_producer<T>(
            [b = std::move(body_)](writer<T> w) mutable {
                b(std::move(w));
            });
    }
};

// Wrapper for a reader→writer transform combinator body.
// spawn(writer) binds the output; spawn(reader) binds the input;
// spawn() (when In == Out) creates both endpoints.
template <typename In, typename Out, typename F>
struct filter {
    F body_;

    void operator()(reader<In> r, writer<Out> w) {
        body_(std::move(r), std::move(w));
    }

    auto bind(reader<In> r, writer<Out> w) const & {
        return [b = body_,
                r = std::move(r), w = std::move(w)]() mutable {
            b(std::move(r), std::move(w));
        };
    }
    auto bind(reader<In> r, writer<Out> w) && {
        return [b = std::move(body_),
                r = std::move(r), w = std::move(w)]() mutable {
            b(std::move(r), std::move(w));
        };
    }

    writer<In> spawn(writer<Out> w) const & {
        return spawn_consumer<In>(
            [b = body_,
             w = std::move(w)](reader<In> r) mutable {
                b(std::move(r), std::move(w));
            });
    }
    writer<In> spawn(writer<Out> w) && {
        return spawn_consumer<In>(
            [b = std::move(body_),
             w = std::move(w)](reader<In> r) mutable {
                b(std::move(r), std::move(w));
            });
    }

    reader<Out> spawn(reader<In> r) const & {
        return spawn_producer<Out>(
            [b = body_,
             r = std::move(r)](writer<Out> w) mutable {
                b(std::move(r), std::move(w));
            });
    }
    reader<Out> spawn(reader<In> r) && {
        return spawn_producer<Out>(
            [b = std::move(body_),
             r = std::move(r)](writer<Out> w) mutable {
                b(std::move(r), std::move(w));
            });
    }

    template <typename T = In>
        requires std::is_same_v<T, Out>
    chan<T> spawn() const & {
        return spawn_filter<T>(
            [b = body_](reader<T> r, writer<T> w) mutable {
                b(std::move(r), std::move(w));
            });
    }
    template <typename T = In>
        requires std::is_same_v<T, Out>
    chan<T> spawn() && {
        return spawn_filter<T>(
            [b = std::move(body_)](reader<T> r, writer<T> w) mutable {
                b(std::move(r), std::move(w));
            });
    }
};

template <typename T, typename F>
consumer<T, std::decay_t<F>> make_consumer(F&& f) {
    return {std::forward<F>(f)};
}

template <typename T, typename F>
producer<T, std::decay_t<F>> make_producer(F&& f) {
    return {std::forward<F>(f)};
}

template <typename In, typename Out = In, typename F>
filter<In, Out, std::decay_t<F>> make_filter(F&& f) {
    return {std::forward<F>(f)};
}

// --- Composition via | ---

// filter | filter → filter
template <typename In, typename Mid, typename F1, typename Out, typename F2>
auto operator|(filter<In, Mid, F1> lhs, filter<Mid, Out, F2> rhs) {
    return make_filter<In, Out>(
        [lhs = std::move(lhs), rhs = std::move(rhs)]
        (reader<In> in, writer<Out> out) mutable {
            rhs(std::move(lhs).spawn(std::move(in)), std::move(out));
        });
}

// producer | filter → producer
template <typename T, typename F1, typename Out, typename F2>
auto operator|(producer<T, F1> lhs, filter<T, Out, F2> rhs) {
    return make_producer<Out>(
        [lhs = std::move(lhs), rhs = std::move(rhs)]
        (writer<Out> out) mutable {
            rhs(std::move(lhs).spawn(), std::move(out));
        });
}

// filter | consumer → consumer
template <typename In, typename Out, typename F1, typename F2>
auto operator|(filter<In, Out, F1> lhs, consumer<Out, F2> rhs) {
    return make_consumer<In>(
        [lhs = std::move(lhs), rhs = std::move(rhs)]
        (reader<In> in) mutable {
            rhs(std::move(lhs).spawn(std::move(in)));
        });
}

// producer | consumer → callable
template <typename T, typename F1, typename F2>
auto operator|(producer<T, F1> lhs, consumer<T, F2> rhs) {
    return [lhs = std::move(lhs), rhs = std::move(rhs)]() mutable {
        rhs(std::move(lhs).spawn());
    };
}

// reader | filter → reader (spawns immediately)
template <typename In, typename Out, typename F>
reader<Out> operator|(reader<In> r, filter<In, Out, F> f) {
    return std::move(f).spawn(std::move(r));
}

// reader | consumer → callable
template <typename T, typename F>
auto operator|(reader<T> r, consumer<T, F> c) {
    return std::move(c).bind(std::move(r));
}

// filter | writer → writer (spawns immediately)
template <typename In, typename Out, typename F>
writer<In> operator|(filter<In, Out, F> f, writer<Out> w) {
    return std::move(f).spawn(std::move(w));
}

// producer | writer → callable
template <typename T, typename F>
auto operator|(producer<T, F> p, writer<T> w) {
    return std::move(p).bind(std::move(w));
}

// --- chan | composition (buffered pipeline stages) ---
// These are all lazy — returning filter/producer/consumer — matching the
// convention that only concrete endpoints (reader/writer) trigger eager
// spawning.

// filter | chan → filter
template <typename T, typename F>
auto operator|(filter<T, T, F> f, chan<T> ch) {
    return make_filter<T>(
        [f = std::move(f), ch = std::move(ch)]
        (reader<T> in, writer<T> out) mutable {
            spawn([f = std::move(f),
                   in = std::move(in),
                   w = std::move(ch.w)]() mutable {
                f(std::move(in), std::move(w));
            });
            csp::detail::pump(std::move(ch.r), std::move(out));
        });
}

// chan | filter → filter
template <typename T, typename F>
auto operator|(chan<T> ch, filter<T, T, F> f) {
    return make_filter<T>(
        [ch = std::move(ch), f = std::move(f)]
        (reader<T> in, writer<T> out) mutable {
            spawn([in = std::move(in),
                   w = std::move(ch.w)]() mutable {
                csp::detail::pump(std::move(in), std::move(w));
            });
            f(std::move(ch.r), std::move(out));
        });
}

// producer | chan → producer
template <typename T, typename F>
auto operator|(producer<T, F> p, chan<T> ch) {
    return make_producer<T>(
        [p = std::move(p), ch = std::move(ch)]
        (writer<T> out) mutable {
            spawn([p = std::move(p),
                   w = std::move(ch.w)]() mutable {
                p(std::move(w));
            });
            csp::detail::pump(std::move(ch.r), std::move(out));
        });
}

// chan | consumer → consumer
template <typename T, typename F>
auto operator|(chan<T> ch, consumer<T, F> c) {
    return make_consumer<T>(
        [ch = std::move(ch), c = std::move(c)]
        (reader<T> in) mutable {
            spawn([in = std::move(in),
                   w = std::move(ch.w)]() mutable {
                csp::detail::pump(std::move(in), std::move(w));
            });
            c(std::move(ch.r));
        });
}

namespace detail {

// Shared machinery for heterogeneous fan-in parts (mux, combine_latest —
// 🎯T49: formerly duplicated per part).

// Set up ChanOp read slots for a tuple of typed inputs, each pointing at
// its typed buffer.
template <size_t... Is, typename Bufs, typename Inputs>
void hetero_read_setup(internal::ChanOp* chanops, Bufs& bufs, Inputs& inputs,
                       std::index_sequence<Is...>) {
    ((chanops[Is] = {internal::wait(std::get<Is>(inputs).internal_reader()),
                     &std::get<Is>(bufs),
                     internal::get_slot(std::get<Is>(inputs).internal_reader().ptr)}), ...);
}

// Typed-transfer dispatch table, built at compile time via index_sequence
// expansion: transfers[i] moves a Ts...[i] value between type-erased
// pointers. Parts derive from this and add their own per-index tables
// (mux: variant wrappers; combine_latest: latest-tuple updaters).
template <typename... Ts>
struct hetero_transfer {
    using xfer_fn = void(*)(void*, void*);

    template <size_t I>
    static void transfer(void* d, void* s) {
        using T = std::tuple_element_t<I, std::tuple<Ts...>>;
        *static_cast<T*>(d) = std::move(*static_cast<T*>(s));
    }

    template <size_t... Is>
    static constexpr auto make_transfers(std::index_sequence<Is...>) {
        return std::array<xfer_fn, sizeof...(Is)>{{&transfer<Is>...}};
    }

    static constexpr auto transfers =
        make_transfers(std::index_sequence_for<Ts...>{});
};

} // namespace detail

}
