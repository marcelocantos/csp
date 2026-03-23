// task_scheduler.cc — Priority task scheduler with worker pool
//
// Demonstrates: request/response (request<Req, Resp>, call, operator()),
// worker pool pattern, progress reporting over channels, and the
// channels-as-interfaces pattern (endpoint bundles).
//
// Architecture:
//   - Dispatcher imp: accepts submit requests, manages priority queue
//   - Worker pool: N worker imps pull tasks and report completion
//   - Progress reporter: prints status updates as tasks complete
//   - Client: submits tasks via operator()

#include "csp.h"

#include <cstdio>
#include <queue>
#include <string>
#include <vector>

using namespace csp;

// --- Task types ---

struct Task {
    int id;
    std::string name;
    int priority;       // higher = more urgent
    int work_ms;        // simulated work duration
};

struct TaskResult {
    int id;
    std::string message;
};

using SubmitReq = request<Task, int>;  // returns assigned task ID

// --- Scheduler endpoint bundle ---
// This is the "channels as interfaces" pattern: the scheduler's public
// surface is a struct of typed endpoints, not a set of methods.

struct SchedulerEndpoints {
    writer<SubmitReq> submit;         // send tasks here
    reader<TaskResult> completions;   // receive completion events here
};

// Priority comparator: higher priority first.
struct TaskCmp {
    bool operator()(const Task& a, const Task& b) const {
        return a.priority < b.priority;
    }
};

static SchedulerEndpoints start_scheduler(int num_workers) {
    chan<SubmitReq> submit_ch;
    chan<Task> work_ch(16);           // buffered: decouple dispatch from workers
    chan<TaskResult> completions_ch;

    // Dispatcher: accepts submissions, assigns IDs, priority-sorts, dispatches.
    spawn([submit_r = std::move(submit_ch.r),
           work_w = std::move(work_ch.w)] {
        std::priority_queue<Task, std::vector<Task>, TaskCmp> queue;
        int next_id = 1;

        SubmitReq req;
        while (submit_r >> req) {
            int id = next_id++;
            req.value.id = id;
            queue.push(std::move(req.value));
            req.reply << id;

            // Dispatch all queued tasks in priority order.
            while (!queue.empty()) {
                work_w << queue.top();
                queue.pop();
            }
        }
        // submit_r died (all clients disconnected) → close work_ch
        // by dropping work_w, which tells workers to exit.
    });

    // Worker pool: each worker pulls tasks, simulates work, reports results.
    for (int i = 0; i < num_workers; ++i) {
        spawn([i, r = work_ch.r.copy(), w = completions_ch.w.copy()] {
            Task task;
            while (r >> task) {
                sleep(std::chrono::milliseconds(task.work_ms));
                char msg[128];
                snprintf(msg, sizeof(msg), "completed by worker %d", i);
                w << TaskResult{task.id, msg};
            }
        });
    }

    // Release shared endpoints — workers hold their own copies.
    work_ch.r = {};
    completions_ch.w = {};

    return {std::move(submit_ch.w), std::move(completions_ch.r)};
}

int main() {
    spawn([] {
        printf("Task Scheduler (3 workers):\n\n");

        auto sched = start_scheduler(3);

        // Progress reporter: prints completions as they arrive.
        spawn([r = std::move(sched.completions)] {
            TaskResult result;
            while (r >> result) {
                printf("  [done] task %d: %s\n", result.id, result.message.c_str());
            }
        });

        // Submit tasks with varying priorities and durations.
        struct { const char* name; int priority; int ms; } tasks[] = {
            {"compile",     5, 100},
            {"lint",        3,  50},
            {"test",        4,  80},
            {"deploy",      1, 120},
            {"notify",      2,  30},
            {"cleanup",     1,  40},
        };

        printf("  Submitting %zu tasks...\n\n", std::size(tasks));

        for (auto& [name, prio, ms] : tasks) {
            // operator() sends the request and blocks for the response.
            int id = sched.submit(Task{0, name, prio, ms});
            printf("  [submit] %-10s (priority=%d, %3dms) -> task %d\n",
                   name, prio, ms, id);
        }

        printf("\n  Waiting for all tasks to complete...\n\n");

        // Drop submit endpoint — dispatcher sees client death, finishes
        // dispatching, closes work channel, workers drain and exit,
        // completions channel closes, reporter exits.  Cleanup cascades
        // through the topology via bidirectional lifecycle observability.
        sched.submit = {};
    });

    schedule();

    printf("\n  All done.\n");
}
