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
        auto source = chan::spawn_enumerate<int>(std::move(nums));

        // map: square each value
        auto squared = chan::spawn_map<int>(source, [](int n) { return n * n; });

        // where: keep only values whose digit sum > 10
        auto filtered = chan::spawn_where<int>(squared, [](int n) {
            int sum = 0;
            for (int v = n; v > 0; v /= 10) sum += v % 10;
            return sum > 10;
        });

        // buffer: decouple producer/consumer with capacity 4
        auto buffered = chan::spawn_buffer<int>(filtered, 4);

        // tee: tap the stream to print what flows through
        channel<int> tap;
        spawn([r = --tap]{
            printf("  [tap] ");
            for (int n; r >> n;) printf("%d ", n);
            printf("\n");
        });
        auto teed = spawn_producer<int>([=, tap = ++tap](writer<int> out) {
            chan::tee(buffered, out, tap)();
        });

        // sink: collect results
        int total = 0;
        int count = 0;
        chan::sink(teed, [&](int n) {
            count++;
            total += n;
        })();

        printf("  %d values passed all stages, sum = %d\n", count, total);
    });

    schedule();
}
