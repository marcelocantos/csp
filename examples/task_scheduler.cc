// task_scheduler.cc — Priority task scheduler with DAG dependencies, timeouts,
//                     worker pool, progress reporting, and Ctrl-C cancellation.
//
// Demonstrates:
//   - Priority queue: dispatcher maintains a max-heap keyed by priority;
//     jobs only enter the ready-queue once all their deps complete.
//   - DAG dependency resolution: dep_tracker unblocks dependents when each
//     job completes, without spawning per-job imps.
//   - Per-job timeout: each worker runs the job under cancellation(timeout),
//     catching csp::timed_out separately from csp::canceled (Ctrl-C).
//   - Progress channel: every status transition posts to a dashboard imp.
//   - Structured cancellation: SIGINT/SIGTERM → top-level cancel scope fires →
//     workers catch csp::canceled, emit "cancelled" status, drain, then exit.
//
// Demo DAG (10 jobs):
//
//   fetch-config (pri 10, 80ms)
//   fetch-schema (pri 9, 120ms)
//   └─ validate  (pri 8, 60ms)   deps: fetch-config, fetch-schema
//   compile-a    (pri 7, 100ms)
//   compile-b    (pri 7, 90ms)
//   └─ link      (pri 6, 70ms)   deps: validate, compile-a, compile-b
//   test-unit    (pri 5, 80ms)   deps: link
//   test-integ   (pri 5, 250ms)  deps: link  timeout=200ms → timed_out
//   package      (pri 4, 60ms)   deps: test-unit, test-integ
//   deploy       (pri 3, 90ms)   deps: package
//
// Run: build/normal/examples/task_scheduler
// Ctrl-C mid-run to see graceful cancellation.

#include "csp.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <vector>

using namespace csp;
using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Data types
// ---------------------------------------------------------------------------

enum class Status { queued, ready, running, done, failed, timed_out, cancelled };

static const char* status_name(Status s) {
    switch (s) {
        case Status::queued:    return "queued   ";
        case Status::ready:     return "ready    ";
        case Status::running:   return "running  ";
        case Status::done:      return "done     ";
        case Status::failed:    return "failed   ";
        case Status::timed_out: return "timed_out";
        case Status::cancelled: return "cancelled";
    }
    return "?";
}

struct Job {
    int id;
    std::string name;
    int priority;                              // higher = more urgent
    std::vector<int> deps;                     // IDs that must complete first
    std::chrono::milliseconds work;            // simulated work duration
    std::chrono::milliseconds timeout;         // per-job deadline (0 = no limit)
};

struct ProgressEvent {
    int id;
    std::string name;
    Status status;
    std::string detail;   // e.g. "worker 2" or "timed out after 200ms"
};

// ---------------------------------------------------------------------------
// Scheduler internals
// ---------------------------------------------------------------------------

// dep_tracker: tracks which jobs are still blocking each pending job.
struct dep_tracker {
    std::map<int, std::set<int>> pending;  // job_id → set of outstanding dep ids

    void add(int job_id, std::vector<int> const& deps) {
        if (deps.empty()) return;
        pending[job_id] = std::set<int>(deps.begin(), deps.end());
    }

    // Record dep_id as complete; returns list of newly unblocked jobs.
    std::vector<int> complete(int dep_id) {
        std::vector<int> unblocked;
        for (auto it = pending.begin(); it != pending.end(); ) {
            it->second.erase(dep_id);
            if (it->second.empty()) {
                unblocked.push_back(it->first);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
        return unblocked;
    }

    // Record dep_id as failed; returns direct dependents (for cascade).
    // Unlike complete(), a single failed dep dooms the waiting job.
    std::vector<int> cancel(int dep_id) {
        std::vector<int> doomed;
        for (auto it = pending.begin(); it != pending.end(); ) {
            if (it->second.count(dep_id)) {
                doomed.push_back(it->first);
                it = pending.erase(it);
            } else {
                ++it;
            }
        }
        return doomed;
    }
};

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

struct SchedulerHandle {
    writer<Job> submit;             // caller submits jobs here
    reader<ProgressEvent> progress; // dashboard reads events here
};

// Completion notification from worker to dispatcher.
// id > 0: job succeeded; id < 0: job failed/timed_out (abs(id) is the job ID).
// Positive IDs unblock dependents; negative IDs cascade failure downward.
struct Completion { int id; bool success; };

static SchedulerHandle start_scheduler(int num_workers) {
    chan<Job> submit_ch;
    chan<Job> ready_ch(32);                // buffered ready queue
    chan<ProgressEvent> progress_ch(64);   // buffered progress feed
    chan<Completion> done_ch(64);          // workers notify dispatcher

    // --- Dispatcher imp ---
    // Receives submitted jobs. Jobs with no deps immediately enter the
    // priority queue. Others wait in dep_tracker until deps complete.
    // On failure, cascade: mark stuck dependents as failed immediately.
    spawn([sub_r   = std::move(submit_ch.r),
           ready_w = std::move(ready_ch.w),
           done_r  = std::move(done_ch.r),
           prog_w  = progress_ch.w.copy()] () mutable {
        auto cmp = [](Job const& a, Job const& b){ return a.priority < b.priority; };
        std::priority_queue<Job, std::vector<Job>, decltype(cmp)> pq(cmp);
        std::map<int, Job> staged;
        dep_tracker deps;

        auto dispatch_ready = [&] {
            while (!pq.empty()) {
                prog_w << ProgressEvent{pq.top().id, pq.top().name, Status::ready, ""};
                if (!(ready_w << pq.top())) return;  // workers gone
                pq.pop();
            }
        };

        // Cascade failure: mark all dependents as failed and recurse.
        std::function<void(int)> cascade_failure = [&](int failed_id) {
            for (int uid : deps.cancel(failed_id)) {
                prog_w << ProgressEvent{uid, staged.at(uid).name,
                                        Status::failed, "dep failed"};
                cascade_failure(uid);
                staged.erase(uid);
            }
        };

        bool sub_alive = true;
        Job job;
        Completion comp;

        while (sub_alive || !staged.empty()) {
            int which;
            if (sub_alive) {
                which = alt(sub_r >> job, done_r >> comp);
            } else {
                // No more submissions; only wait for completions.
                if (!(done_r >> comp)) break;
                which = 1;
            }

            if (which == 0) {
                prog_w << ProgressEvent{job.id, job.name, Status::queued, ""};
                if (job.deps.empty()) {
                    pq.push(job);
                } else {
                    deps.add(job.id, job.deps);
                    staged[job.id] = job;
                }
                dispatch_ready();
            } else if (which == 1) {
                if (comp.success) {
                    for (int uid : deps.complete(comp.id)) {
                        pq.push(std::move(staged.at(uid)));
                        staged.erase(uid);
                    }
                    dispatch_ready();
                } else {
                    // Job failed/timed_out: cascade failure to all stuck dependents.
                    cascade_failure(comp.id);
                }
            } else if (which == ~0) {
                // submit endpoint closed; keep waiting for completions.
                sub_alive = false;
            }
            // ~1 = done_r closed: workers gone; exit loop.
        }
    });

    // --- Worker pool ---
    for (int w = 0; w < num_workers; ++w) {
        spawn([w, r = ready_ch.r.copy(),
               done_w = done_ch.w.copy(),
               prog_w = progress_ch.w.copy()] () mutable {
            Job job;
            while (r >> job) {
                prog_w << ProgressEvent{job.id, job.name, Status::running,
                                        "worker " + std::to_string(w)};
                Status final_status = Status::done;
                std::string detail = "worker " + std::to_string(w);
                try {
                    // Apply per-job timeout if specified; otherwise inherit
                    // the parent cancel scope (top-level Ctrl-C).
                    if (job.timeout.count() > 0) {
                        auto tg = cancellation(job.timeout);
                        sleep(job.work);
                    } else {
                        sleep(job.work);
                    }
                } catch (timed_out const&) {
                    final_status = Status::timed_out;
                    detail = "after " + std::to_string(job.timeout.count()) + "ms";
                } catch (canceled const&) {
                    final_status = Status::cancelled;
                    detail = "Ctrl-C";
                } catch (std::exception const& e) {
                    final_status = Status::failed;
                    detail = e.what();
                }
                prog_w << ProgressEvent{job.id, job.name, final_status, detail};
                // Notify dispatcher with success/failure so it can unblock or
                // cascade-fail dependents accordingly.
                done_w << Completion{job.id, final_status == Status::done};
            }
        });
    }

    // Release shared endpoints — workers hold their own copies.
    ready_ch.r = {};
    done_ch.w = {};

    return {std::move(submit_ch.w), std::move(progress_ch.r)};
}

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------

static void run_dashboard(reader<ProgressEvent> prog) {
    spawn([r = std::move(prog)] {
        ProgressEvent ev;
        while (r >> ev) {
            printf("  [%s] #%-2d %-12s %s\n",
                   status_name(ev.status), ev.id,
                   ev.name.c_str(), ev.detail.c_str());
            fflush(stdout);
        }
    });
}

// ---------------------------------------------------------------------------
// Demo DAG
// ---------------------------------------------------------------------------

int main() {
    spawn([] {
        printf("Task Scheduler — priority queue + DAG deps + per-job timeout\n");
        printf("(Send SIGINT / Ctrl-C to trigger graceful cancellation)\n\n");

        // Top-level cancel scope: fires when guard() is called.
        // Use shared_ptr so the signal watcher can safely call it after the
        // outer imp has moved on to submitting jobs.
        auto guard_ptr = std::make_shared<cancel_guard>(cancellation());

        // Signal watcher: fires guard on SIGINT/SIGTERM.
        auto sig = signal::notify({SIGINT, SIGTERM});
        spawn([sig_r = std::move(sig), gp = guard_ptr] () mutable {
            int s;
            if (sig_r >> s) {
                printf("\n  [signal] caught %d — cancelling all jobs\n\n", s);
                fflush(stdout);
                (*gp)();
            }
        });

        auto sched = start_scheduler(3);
        run_dashboard(std::move(sched.progress));

        // Submit the demo DAG.
        //
        //   fetch-config (10)
        //   fetch-schema (9)
        //   └─ validate  (8)   deps: 1,2
        //   compile-a    (7)
        //   compile-b    (7)
        //   └─ link      (6)   deps: 3,4,5
        //   test-unit    (5)   deps: 6
        //   test-integ   (5)   deps: 6   timeout=200ms, work=250ms → timed_out
        //   package      (4)   deps: 7,8
        //   deploy       (3)   deps: 9

        struct JobSpec {
            int id;
            const char* name;
            int priority;
            std::vector<int> deps;
            int work_ms;
            int timeout_ms;   // 0 = no limit
        };

        JobSpec specs[] = {
            {1,  "fetch-config", 10, {},        80,  0},
            {2,  "fetch-schema",  9, {},       120,  0},
            {3,  "validate",      8, {1,2},     60,  0},
            {4,  "compile-a",     7, {},       100,  0},
            {5,  "compile-b",     7, {},        90,  0},
            {6,  "link",          6, {3,4,5},   70,  0},
            {7,  "test-unit",     5, {6},        80,  0},
            {8,  "test-integ",    5, {6},       250, 200},  // will time out
            {9,  "package",       4, {7,8},      60,  0},
            {10, "deploy",        3, {9},        90,  0},
        };

        printf("  Submitting %zu jobs...\n\n", std::size(specs));

        for (auto& s : specs) {
            sched.submit << Job{s.id, s.name, s.priority, s.deps,
                                std::chrono::milliseconds(s.work_ms),
                                std::chrono::milliseconds(s.timeout_ms)};
        }

        // Dropping submit lets the dispatcher know no more jobs are coming.
        // Dispatcher drains, workers drain, progress_ch closes, dashboard exits.
        sched.submit = {};
    });

    schedule();

    printf("\n  Scheduler done.\n");
}
