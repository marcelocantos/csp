// pipeline.cc — Stream combinator composition
//
// Build a data processing pipeline from composable combinators,
// just like Unix pipes: source | map | where | buffer | tee | sink.
// Each stage runs as its own microthread with zero shared state.
// The combinators (map, where, buffer, tee, sink, enumerate) are
// all provided by the library — you just snap them together.

#include <csp/microthread.h>
#include <csp/enumerate.h>
#include <csp/map.h>
#include <csp/where.h>
#include <csp/buffer.h>
#include <csp/tee.h>
#include <csp/sink.h>

#include <cstdio>
#include <vector>

using namespace csp;

int main() {
    spawn([]{
        // Source: integers 1..50
        std::vector<int> nums(50);
        for (int i = 0; i < 50; ++i) nums[i] = i + 1;
        auto source = spawn_enumerate<int>(std::move(nums));

        // map: square each value
        auto squared = spawn_map<int>(std::move(source), [](int n) { return n * n; });

        // where: keep only values whose digit sum > 10
        auto filtered = spawn_where<int>(std::move(squared), [](int n) {
            int sum = 0;
            for (int v = n; v > 0; v /= 10) sum += v % 10;
            return sum > 10;
        });

        // buffer: decouple producer/consumer with capacity 4
        auto buffered = spawn_buffer<int>(std::move(filtered), 4);

        // tee: tap the stream to print what flows through
        chan<int> tap;
        spawn([r = std::move(tap.r)]{
            printf("  [tap] ");
            for (int n; r >> n;) printf("%d ", n);
            printf("\n");
        });
        auto teed = spawn_producer<int>([buffered = std::move(buffered), tap = std::move(tap.w)](writer<int> out) mutable {
            tee(std::move(buffered), std::move(out), std::move(tap))();
        });

        // sink: collect results
        int total = 0;
        int count = 0;
        sink(std::move(teed), [&](int n) {
            count++;
            total += n;
        })();

        printf("  %d values passed all stages, sum = %d\n", count, total);
    });

    schedule();
}
