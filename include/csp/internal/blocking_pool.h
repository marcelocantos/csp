#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace csp::detail {

struct Imp;

class BlockingPool {
public:
    static BlockingPool& instance();

    // Submit a function to run on a pool thread. The imp will be
    // rescheduled via imp->schedule() when the function completes.
    void submit(Imp* imp, std::function<void()> fn);

    // Lazy init: creates worker threads on first submit.
    void ensure_started();

    // Drain queue and join all workers. Idempotent.
    void shutdown();

private:
    BlockingPool() = default;
    BlockingPool(BlockingPool const&) = delete;
    BlockingPool& operator=(BlockingPool const&) = delete;

    struct Work {
        Imp* imp;
        std::function<void()> fn;
    };

    void worker();

    std::vector<std::thread> threads_;
    std::vector<Work> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::mutex start_mu_;
};

}
