// 10_pipeline.cpp — Declarative stream processing with combinators
//
// Build a data pipeline from snap-together parts: source | map |
// where | batch. Each stage runs as its own concurrent imp with
// automatic backpressure between stages — no manual queue sizing,
// no thread pools, no shared state.

#include "csp.h"

#include <cstdio>
#include <vector>

using namespace csp;
using namespace csp::part;

int main() {
    spawn([]{
        std::vector<int> data(30);
        for (int i = 0; i < 30; ++i) data[i] = i + 1;

        // source → square → keep evens → groups of 4
        auto source   = enumerate(std::move(data)).spawn();
        auto squared  = map<int>([](int n){ return n * n; }).spawn(std::move(source));
        auto evens    = where<int>([](int n){ return n % 2 == 0; }).spawn(std::move(squared));
        auto batched  = batch<int>(4).spawn(std::move(evens));

        printf("  squares of 1..30, even only, in groups of 4:\n");
        for (std::vector<int> group; batched >> group;) {
            printf("    [");
            for (size_t i = 0; i < group.size(); ++i)
                printf("%s%d", i ? ", " : "", group[i]);
            printf("]\n");
        }
    });

    schedule();
}
