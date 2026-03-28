#pragma once

#include <csp/part/part.h>

#include <functional>
#include <map>

namespace csp::part {

// Reorder an out-of-sequence stream back into key order.
//
// key_fn extracts a sortable key from each value. Values are buffered
// until contiguous keys can be emitted.  Expects keys to form a
// contiguous ascending sequence starting from initial_key (default 0).
//
// Example: parallel_map may produce results out of order.
//   source | enumerate<T> | parallel_map<...>(f) | reorder<pair<size_t,R>>(first)
// restores the original ordering.
//
// Buffer is bounded by the maximum out-of-order distance. If the
// source is nearly ordered, memory usage is low.
template <typename T, typename Key = size_t>
auto reorder(std::function<Key(T const&)> key_fn, Key initial_key = Key{}) {
    return make_filter<T, T>(
        [key_fn = std::move(key_fn), initial_key](reader<T> in, writer<T> out) {
            internal::descr("reorder");
            std::map<Key, T> buf;
            Key next = initial_key;
            for (T t; csp::alt(in >> t, ~out) == 0;) {
                Key k = key_fn(t);
                if (k == next) {
                    // In order — emit directly.
                    if (!(out << std::move(t))) return;
                    ++next;
                    // Flush any buffered values that are now contiguous.
                    while (true) {
                        auto it = buf.find(next);
                        if (it == buf.end()) break;
                        if (!(out << std::move(it->second))) return;
                        buf.erase(it);
                        ++next;
                    }
                } else {
                    // Out of order — buffer.
                    buf.emplace(k, std::move(t));
                }
            }
            // Flush remaining buffered values in key order.
            for (auto& [k, v] : buf) {
                if (!(out << std::move(v))) return;
            }
        });
}

}
