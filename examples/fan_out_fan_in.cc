// fan_out_fan_in.cc — Parallel work distribution
//
// Distribute jobs across N workers, collect results into one stream.
// Each worker is an independent microthread — no shared queues, no
// mutexes, no counting semaphores. Workers pull from a shared input
// channel and push to a shared output channel. When the input is
// exhausted, workers exit naturally and the output channel closes.
//
// Compare to thread pool + mutex + condition variable boilerplate.

#include <csp/csp.h>

#include <cstdio>
#include <cmath>
#include <algorithm>

using namespace csp;

int main() {
    spawn([]{
        constexpr int NUM_JOBS = 20;
        constexpr int NUM_WORKERS = 4;

        struct Job { int id; int value; };
        struct Result { int id; double answer; };

        // Job source
        auto [jobs_w, jobs_r] = chan<Job>{};
        spawn([w = std::move(jobs_w)]{
            for (int i = 0; i < NUM_JOBS; ++i) {
                if (!(w << Job{i, (i + 1) * 7})) return;
            }
        });

        // Results channel — shared by all workers.
        // Move the writer out so workers hold the only refs.
        auto [results_w, results_r] = chan<Result>{};

        // Spawn workers. Each pulls from jobs, computes, pushes to results.
        // When all workers exit, the results writer refcount drops to zero
        // and the reader closes.
        for (int w = 0; w < NUM_WORKERS; ++w) {
            spawn([jobs_r = jobs_r.copy(), out = results_w.copy()]{
                for (Job j; jobs_r >> j;) {
                    // "Heavy computation"
                    double answer = std::sqrt(static_cast<double>(j.value));
                    if (!(out << Result{j.id, answer})) return;
                }
            });
        }
        results_w = {};  // Release our copy; workers hold the only refs.

        // Collect results
        printf("Fan-out to %d workers, %d jobs:\n", NUM_WORKERS, NUM_JOBS);
        std::vector<Result> collected;
        for (Result r; results_r >> r;) {
            collected.push_back(r);
        }
        std::sort(collected.begin(), collected.end(),
                  [](auto & a, auto & b) { return a.id < b.id; });

        for (auto & r : collected) {
            printf("  job %2d: sqrt(%d) = %.4f\n", r.id, (r.id + 1) * 7, r.answer);
        }
    });

    schedule();
}
