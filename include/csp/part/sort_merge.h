#pragma once

#include <csp/part/part.h>

#include <functional>
#include <queue>
#include <vector>

namespace csp::part {

// Merge N pre-sorted streams into one sorted output.
// Each input must be sorted according to cmp. Output is the sorted merge.
template <typename T, typename Cmp = std::less<T>>
auto sort_merge(std::vector<reader<T>> inputs, Cmp cmp = {}) {
    return make_producer<T>(
        [inputs = std::move(inputs), cmp = std::move(cmp)]
        (writer<T> out) mutable {
            internal::descr("sort_merge");

            // Heap entry: (value, index into inputs).
            struct Entry {
                T value;
                size_t idx;
            };
            auto heap_cmp = [&cmp](Entry const& a, Entry const& b) {
                return cmp(b.value, a.value); // reverse for min-heap
            };
            std::priority_queue<Entry, std::vector<Entry>, decltype(heap_cmp)>
                heap(heap_cmp);

            // Prime: read one value from each input.
            for (size_t i = 0; i < inputs.size(); ++i) {
                T t;
                if (csp::alt(inputs[i] >> t, ~out) == 0)
                    heap.push({std::move(t), i});
            }

            while (!heap.empty()) {
                auto [value, idx] = std::move(const_cast<Entry&>(heap.top()));
                heap.pop();
                if (!(out << std::move(value))) return;

                // Refill from the same input.
                T t;
                if (csp::alt(inputs[idx] >> t, ~out) == 0)
                    heap.push({std::move(t), idx});
            }
        });
}

}
