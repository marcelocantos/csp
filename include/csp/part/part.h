#pragma once

#include <csp/csp.h>

#include <type_traits>
#include <utility>

namespace csp::part {

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

}
