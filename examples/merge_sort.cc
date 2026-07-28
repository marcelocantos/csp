// merge_sort.cc — Parallel divide & conquer
//
// Recursive merge sort where each merge step uses channels for
// synchronization. The merge function reads from two sorted input
// channels, advancing whichever side holds the smaller value, and
// writes the merged order to one output channel.
//
// This demonstrates recursive spawn and channel-mediated data flow.
// The parallelism is natural — no explicit thread management, no
// barriers, no shared mutable state.

#include "csp.h"

#include <cstdio>
#include <vector>
#include <algorithm>

using namespace csp;
using namespace csp::part;

// Merge two sorted channels into one.
void merge(reader<int> left, reader<int> right, writer<int> out) {
    int l, r;
    bool have_l = bool(left >> l);
    bool have_r = bool(right >> r);

    while (have_l && have_r) {
        if (l <= r) {
            if (!(out << l)) return;
            have_l = bool(left >> l);
        } else {
            if (!(out << r)) return;
            have_r = bool(right >> r);
        }
    }
    while (have_l) {
        if (!(out << l)) return;
        have_l = bool(left >> l);
    }
    while (have_r) {
        if (!(out << r)) return;
        have_r = bool(right >> r);
    }
}

// Recursively sort: returns a reader that emits sorted values.
reader<int> parallel_sort(std::vector<int> data) {
    if (data.size() <= 1) {
        return spawn_producer<int>([data = std::move(data)](writer<int> out) {
            for (int v : data) {
                if (!(out << v)) return;
            }
        });
    }

    size_t mid = data.size() / 2;
    auto left  = parallel_sort({data.begin(), data.begin() + mid});
    auto right = parallel_sort({data.begin() + mid, data.end()});

    return spawn_producer<int>([left = std::move(left), right = std::move(right)](writer<int> out) mutable {
        merge(std::move(left), std::move(right), std::move(out));
    });
}

int main() {
    spawn([]{
        std::vector<int> data = {38, 27, 43, 3, 9, 82, 10, 1, 44, 17, 5, 99, 23, 61, 8, 42};

        printf("Merge sort (%zu elements):\n  Input:  ", data.size());
        for (int v : data) printf("%d ", v);
        printf("\n");

        auto sorted = parallel_sort(data);

        printf("  Output: ");
        for (int v; sorted >> v;) {
            printf("%d ", v);
        }
        printf("\n");
    });

    await_completion();
}
