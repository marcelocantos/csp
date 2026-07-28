#pragma once

#include <csp/part/part.h>

#include <algorithm>
#include <random>
#include <utility>
#include <vector>

namespace csp::part::rand {

namespace detail {

// Shared skeleton for the distribution-backed producers: an infinite
// stream of dist(eng) draws.  `dist` is any callable taking Engine&.
template <typename T, typename Dist, typename Engine>
auto from_distribution(char const* name, Dist dist, Engine eng) {
    return make_producer<T>(
        [name, dist = std::move(dist),
         eng = std::move(eng)](writer<T> sink) mutable {
            internal::descr(name);
            while (sink << dist(eng)) { }
        });
}

}

// Infinite stream of uniform random integers in [lo, hi].
template <typename T, typename Engine = std::mt19937_64>
auto uniform_int(T lo, T hi,
                 Engine eng = Engine{std::random_device{}()}) {
    return detail::from_distribution<T>(
        "uniform_int", std::uniform_int_distribution<T>(lo, hi),
        std::move(eng));
}

// Infinite stream of uniform random reals in [lo, hi).
template <typename T, typename Engine = std::mt19937_64>
auto uniform_real(T lo, T hi,
                  Engine eng = Engine{std::random_device{}()}) {
    return detail::from_distribution<T>(
        "uniform_real", std::uniform_real_distribution<T>(lo, hi),
        std::move(eng));
}

// Infinite stream of random bools with P(true) = p.
template <typename Engine = std::mt19937_64>
auto bernoulli(double p = 0.5,
               Engine eng = Engine{std::random_device{}()}) {
    return detail::from_distribution<bool>(
        "bernoulli", std::bernoulli_distribution(p), std::move(eng));
}

// Infinite stream of normally distributed values.
template <typename T = double, typename Engine = std::mt19937_64>
auto normal(T mean = 0, T stddev = 1,
            Engine eng = Engine{std::random_device{}()}) {
    return detail::from_distribution<T>(
        "normal", std::normal_distribution<T>(mean, stddev),
        std::move(eng));
}

// Infinite stream of random picks from a container: from_distribution
// over an index distribution into the container.
template <typename T, typename C, typename Engine = std::mt19937_64>
auto choice(C&& c, Engine eng = Engine{std::random_device{}()}) {
    size_t const hi = c.size() - 1;
    return detail::from_distribution<T>(
        "choice",
        [c = std::forward<C>(c),
         dist = std::uniform_int_distribution<size_t>(0, hi)](
            Engine& e) mutable { return c[dist(e)]; },
        std::move(eng));
}

template <typename T, typename Engine = std::mt19937_64>
auto choice(std::initializer_list<T> c,
            Engine eng = Engine{std::random_device{}()}) {
    return choice<T>(std::vector<T>(c), std::move(eng));
}

// Infinite stream of random byte chunks of the given size.
// Not folded into from_distribution: it refills and re-sends one shared
// buffer rather than drawing a fresh value per send.
template <typename Engine = std::mt19937_64>
auto random_bytes(size_t chunk_size,
                  Engine eng = Engine{std::random_device{}()}) {
    return make_producer<bytes>(
        [chunk_size, eng = std::move(eng)](
            writer<bytes> sink) mutable {
            internal::descr("random_bytes");
            std::uniform_int_distribution<unsigned> dist(0, 255);
            bytes buf(chunk_size);
            for (;;) {
                for (auto& b : buf) b = static_cast<uint8_t>(dist(eng));
                if (!(sink << buf)) return;
            }
        });
}

// Reservoir shuffle: buffer n elements, then for each new input pick a
// random slot, emit the displaced element, and store the new one.  On
// input exhaustion, Fisher-Yates shuffle the remaining buffer and emit.
template <typename T, typename Engine = std::mt19937_64>
auto shuffle(size_t n, Engine eng = Engine{std::random_device{}()}) {
    return make_filter<T>(
        [n, eng = std::move(eng)](reader<T> in, writer<T> out) mutable {
            internal::descr("shuffle");

            // Fill the buffer.
            std::vector<T> buf;
            buf.reserve(n);
            for (T v; buf.size() < n && (in >> v);)
                buf.push_back(std::move(v));

            // Steady state: swap-and-emit.
            std::uniform_int_distribution<size_t> dist(0, buf.size() - 1);
            for (T v; csp::alt(in >> v, ~out) == 0;) {
                size_t idx = dist(eng);
                if (!(out << std::move(buf[idx]))) return;
                buf[idx] = std::move(v);
            }

            // Drain: Fisher-Yates shuffle remaining buffer.
            for (size_t i = buf.size(); i > 1; --i) {
                std::uniform_int_distribution<size_t> d(0, i - 1);
                std::swap(buf[i - 1], buf[d(eng)]);
            }
            for (auto& v : buf) {
                if (!(out << std::move(v))) return;
            }
        });
}

}
