// pipeline.cc — Stream combinator composition
//
// Build a data processing pipeline from composable combinators,
// just like Unix pipes: source | map | where | buffer | tee | sink.
// Each stage runs as its own microthread with zero shared state.
// The combinators (map, where, buffer, tee, sink, enumerate) are
// all provided by the library — you just snap them together.

#include <csp/csp.h>
#include <csp/part/enumerate.h>
#include <csp/part/map.h>
#include <csp/part/where.h>
#include <csp/part/buffer.h>
#include <csp/part/tee.h>
#include <csp/part/sink.h>

#include <cstdio>
#include <vector>

using namespace csp;
using namespace csp::part;

int main() {
    spawn([]{
        // Source: integers 1..50
        std::vector<int> nums(50);
        for (int i = 0; i < 50; ++i) nums[i] = i + 1;
        auto source = enumerate<int>(std::move(nums)).spawn();

        // map: square each value
        auto squared = map<int>([](int n) { return n * n; }).spawn(std::move(source));

        // where: keep only values whose digit sum > 10
        auto filtered = where<int>([](int n) {
            int sum = 0;
            for (int v = n; v > 0; v /= 10) sum += v % 10;
            return sum > 10;
        }).spawn(std::move(squared));

        // buffer: decouple producer/consumer with capacity 4
        auto buffered = buffer<int>(4).spawn(std::move(filtered));

        // tee: tap the stream to print what flows through
        auto [tap_w, tap_r] = chan<int>{};
        spawn([r = std::move(tap_r)]{
            printf("  [tap] ");
            for (int n; r >> n;) printf("%d ", n);
            printf("\n");
        });
        auto teed = tee<int>(std::move(tap_w)).spawn(std::move(buffered));

        // sink: collect results
        int total = 0;
        int cnt = 0;
        sink<int>([&](int n) {
            cnt++;
            total += n;
        })(std::move(teed));

        printf("  %d values passed all stages, sum = %d\n", cnt, total);
    });

    schedule();
}
