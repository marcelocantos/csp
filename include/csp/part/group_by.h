#pragma once

#include <csp/part/part.h>

#include <type_traits>
#include <unordered_map>
#include <utility>

namespace csp::part {

// Dynamic partitioning: each new key lazily spawns a new sub-stream.
// Emits (key, reader<T>) pairs on a meta-channel. Values for known keys
// are forwarded to the existing sub-stream. If a sub-stream reader is
// dropped, future values for that key are discarded.
//
// Eager by design: group_by returns live endpoints (a reader of
// (key, reader<T>) pairs backed by an already-spawned imp), not a lazy
// make_* part.  The lazy convention (see part.h) does not apply —
// converting would be a user-visible breaking change.
template <typename T, typename F,
          typename K = std::decay_t<std::invoke_result_t<F&, const T&>>>
reader<std::pair<K, reader<T>>> group_by(reader<T> input, F f) {
    return spawn_producer<std::pair<K, reader<T>>>(
        [input = std::move(input), f = std::move(f)]
        (writer<std::pair<K, reader<T>>> meta) mutable {
            internal::descr("group_by");
            std::unordered_map<K, writer<T>> groups;

            for (T t; input >> t;) {
                K key = f(t);
                auto it = groups.find(key);
                if (it == groups.end()) {
                    chan<T> ch;
                    if (!(meta << std::pair<K, reader<T>>{
                              key, std::move(ch.r)}))
                        return;
                    it = groups.emplace(key, std::move(ch.w)).first;
                }
                it->second << std::move(t);
            }
        });
}

}
