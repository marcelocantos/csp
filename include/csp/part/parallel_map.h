#pragma once

#include <csp/part/part.h>

#include <flat_map>
#include <type_traits>
#include <utility>

namespace csp::part {

struct parallel_map_config {
    bool ordered = false;
};

// Concurrent transform: fan out to N workers, reassemble in input order
// (ordered=true) or emit as completed (ordered=false, default).
// Demand-driven dispatch: whichever worker is free reads next.
template <typename A, typename B = A, typename F>
auto parallel_map(size_t n, F&& f, parallel_map_config cfg = {}) {
    return make_filter<A, B>(
        [n, f = std::forward<F>(f), cfg](reader<A> in, writer<B> out) {
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
                    csp::spawn(
                        [f, num_r = num_r.copy(), res_w = res_w.copy()]() mutable {
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
                std::flat_map<size_t, B> buf;
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
                // done channel tracks worker completion: when all workers
                // exit their done_w copies are dropped, making done_r dead.
                auto [done_w, done_r] = chan<>{};

                for (size_t i = 0; i < n; ++i) {
                    csp::spawn(
                        [f, in = in.copy(), out = out.copy(),
                         done_w = done_w.copy()]() mutable {
                            internal::descr("parallel_map/worker");
                            for (;;) {
                                A a;
                                // Watch for output death while waiting for input.
                                if (csp::alt(in >> a, ~out) != 0) return;
                                if (!(out << f(std::move(a)))) return;
                            }
                        });
                }
                // Drop originals — workers hold the live copies.
                in = {};
                done_w = {};

                // Block until all workers finish or output dies.
                csp::alt(~done_r, ~out);
            }
        });
}

}
