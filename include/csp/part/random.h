#pragma once

#include <csp/part/part.h>

#include <algorithm>
#include <random>
#include <vector>

namespace csp::part::rand {

// Infinite stream of uniform random integers in [lo, hi].
template <typename T, typename Engine = std::mt19937_64>
auto uniform_int(T lo, T hi,
                 Engine eng = Engine{std::random_device{}()}) {
    return make_producer<T>(
        [lo, hi, eng = std::move(eng)](writer<T> sink) mutable {
            internal::descr("uniform_int");
            std::uniform_int_distribution<T> dist(lo, hi);
            while (sink << dist(eng)) { }
        });
}

// Infinite stream of uniform random reals in [lo, hi).
template <typename T, typename Engine = std::mt19937_64>
auto uniform_real(T lo, T hi,
                  Engine eng = Engine{std::random_device{}()}) {
    return make_producer<T>(
        [lo, hi, eng = std::move(eng)](writer<T> sink) mutable {
            internal::descr("uniform_real");
            std::uniform_real_distribution<T> dist(lo, hi);
            while (sink << dist(eng)) { }
        });
}

// Infinite stream of random bools with P(true) = p.
template <typename Engine = std::mt19937_64>
auto bernoulli(double p = 0.5,
               Engine eng = Engine{std::random_device{}()}) {
    return make_producer<bool>(
        [p, eng = std::move(eng)](writer<bool> sink) mutable {
            internal::descr("bernoulli");
            std::bernoulli_distribution dist(p);
            while (sink << dist(eng)) { }
        });
}

// Infinite stream of normally distributed values.
template <typename T = double, typename Engine = std::mt19937_64>
auto normal(T mean = 0, T stddev = 1,
            Engine eng = Engine{std::random_device{}()}) {
    return make_producer<T>(
        [mean, stddev, eng = std::move(eng)](writer<T> sink) mutable {
            internal::descr("normal");
            std::normal_distribution<T> dist(mean, stddev);
            while (sink << dist(eng)) { }
        });
}

// Infinite stream of random picks from a container.
template <typename T, typename C, typename Engine = std::mt19937_64>
auto choice(C&& c, Engine eng = Engine{std::random_device{}()}) {
    return make_producer<T>(
        [c = std::forward<C>(c), eng = std::move(eng)](
            writer<T> sink) mutable {
            internal::descr("choice");
            std::uniform_int_distribution<size_t> dist(0, c.size() - 1);
            while (sink << c[dist(eng)]) { }
        });
}

template <typename T, typename Engine = std::mt19937_64>
auto choice(std::initializer_list<T> c,
            Engine eng = Engine{std::random_device{}()}) {
    return choice<T>(std::vector<T>(c), std::move(eng));
}

// Infinite stream of random byte chunks of the given size.
template <typename Engine = std::mt19937_64>
auto random_bytes(size_t chunk_size,
                  Engine eng = Engine{std::random_device{}()}) {
    return make_producer<std::vector<uint8_t>>(
        [chunk_size, eng = std::move(eng)](
            writer<std::vector<uint8_t>> sink) mutable {
            internal::descr("random_bytes");
            std::uniform_int_distribution<unsigned> dist(0, 255);
            std::vector<uint8_t> buf(chunk_size);
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
