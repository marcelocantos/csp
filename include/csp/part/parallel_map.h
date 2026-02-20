#pragma once

#include <csp/part/part.h>

#include <map>
#include <type_traits>
#include <utility>

namespace csp::part {

struct parallel_map_config {
    bool ordered = false;
};

// Ordered concurrent transform: fan out to N workers, reassemble in input
// order (ordered=true) or emit as completed (ordered=false, default).
// Demand-driven: whichever worker is free reads next.
template <typename A, typename B = std::invoke_result_t<std::decay_t<A>&&>, typename F>
auto parallel_map(size_t n, F&& f, parallel_map_config cfg = {}) {
    return make_filter<A, B>(
        [n, f = std::forward<F>(f), cfg](reader<A> in, writer<B> out) mutable {
            internal::descr("parallel_map");

            if (cfg.ordered) {
                // Numbered work items and results.
                auto [num_w, num_r] = chan<std::pair<size_t, A>>{};
                auto [res_w, res_r] = chan<std::pair<size_t, B>>{};

                // Dispatcher: read input, assign sequence numbers.
                csp::spawn([in = std::move(in), num_w = std::move(num_w)]() mutable {
                    internal::descr("parallel_map/dispatch");
                    size_t seq = 0;
                    for (A a; in >> a;) {
                        if (!(num_w << std::make_pair(seq++, std::move(a)))) return;
                    }
                });

                // Workers: read numbered items, apply f, write numbered results.
                for (size_t i = 0; i < n; ++i) {
                    csp::spawn([f, num_r = num_r.copy(), res_w = res_w.copy()]() mutable {
                        internal::descr("parallel_map/worker");
                        std::pair<size_t, A> item;
                        while (num_r >> item) {
                            auto result = f(std::move(item.second));
                            if (!(res_w << std::make_pair(item.first, std::move(result))))
                                return;
                        }
                    });
                }
                // Drop originals so channels close when workers finish.
                num_r = {};
                res_w = {};

                // Collector: reorder results and emit in sequence.
                std::map<size_t, B> buf;
                size_t next = 0;
                for (std::pair<size_t, B> r; csp::alt(res_r >> r, ~out) == 0;) {
                    buf.emplace(r.first, std::move(r.second));
                    while (buf.count(next)) {
                        auto it = buf.find(next);
                        if (!(out << std::move(it->second))) return;
                        buf.erase(it);
                        ++next;
                    }
                }
                // Drain any remaining buffered items.
                while (buf.count(next)) {
                    auto it = buf.find(next);
                    if (!(out << std::move(it->second))) return;
                    buf.erase(it);
                    ++next;
                }
            } else {
                // Unordered: workers share input and output directly.
                for (size_t i = 0; i < n; ++i) {
                    csp::spawn([f, in = in.copy(), out = out.copy()]() mutable {
                        internal::descr("parallel_map/worker");
                        for (A a; in >> a;) {
                            if (!(out << f(std::move(a)))) return;
                        }
                    });
                }
                // Drop originals — workers hold the live copies.
                in = {};
                // Keep out alive until all workers finish by watching for
                // input exhaustion via the workers' shared out copies.
                // The producer framework owns out; just wait for workers.
                // Block until out's reader dies (downstream closed) or
                // all workers finish (they drop their out copies, but we
                // still hold ours). Use a sentinel: drop our in, workers
                // run to completion, drop their outs, then our out is the
                // last copy — but we need to detect that. Simplest: just
                // wait for our writer to become the sole copy by blocking
                // on ~out (reader death).
                //
                // Actually: the producer framework already owns `out`.
                // When we return, `out` is destroyed, closing the output.
                // We just need to wait for all workers to finish. But we
                // can't join microthreads. Instead, the workers share
                // `out` via copy() — when the last worker finishes, the
                // last copy is our local `out`. The producer framework
                // handles closure. We just need to not return early.
                //
                // Wait for output reader death (downstream done).
                poke_t p;
                csp::alt(~out);
            }
        });
}

}
