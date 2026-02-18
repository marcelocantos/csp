#pragma once

#include <csp/internal/flat_hash_set.h>
#include <csp/part/part.h>
#include <csp/ringbuffer.h>

#include <functional>
#include <optional>

namespace csp::part {

// Suppress all-time duplicate values using a flat hash set.
// max_remembered == 0 (default): unbounded — every unique value remembered.
// max_remembered > 0: FIFO eviction — oldest remembered value is forgotten
// when the set reaches capacity, allowing it to pass through again later.
template <typename T, typename Hash = std::hash<T>,
          typename Eq = std::equal_to<T>>
auto unique(size_t max_remembered = 0, Hash hash = {}, Eq eq = {}) {
    return make_filter<T>(
        [max_remembered, hash, eq](reader<T> in, writer<T> out) {
            internal::descr("unique");
            csp::detail::FlatHashSet<T, Hash, Eq> seen(16, hash, eq);
            std::optional<csp::detail::RingBuffer<T>> order;
            if (max_remembered > 0)
                order.emplace(max_remembered);

            for (T t; csp::alt(in >> t, ~out) == 0;) {
                if (seen.insert(t)) {
                    if (order) {
                        if (order->full()) {
                            seen.erase(order->front());
                            order->pop();
                        }
                        order->push(t);
                    }
                    if (!(out << std::move(t))) return;
                }
            }
        });
}

}
