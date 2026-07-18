#include <csp/internal/blocking_pool.h>
#include <csp/internal/csp_internal.h>

#include <cassert>

namespace csp::detail {

BlockingPool& BlockingPool::instance() {
    static BlockingPool p;
    return p;
}

void BlockingPool::ensure_started() {
    if (running_.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lk(start_mu_);
    if (running_.load(std::memory_order_relaxed)) return;

    stopping_.store(false, std::memory_order_relaxed);

    // Use a small fixed pool. These threads only run blocking OS calls,
    // so they spend most of their time in the kernel — not burning CPU.
    size_t n = std::max(4u, std::thread::hardware_concurrency());
    threads_.reserve(n);
    for (size_t i = 0; i < n; ++i)
        threads_.emplace_back([this] { worker(); });

    running_.store(true, std::memory_order_release);
}

void BlockingPool::shutdown() {
    if (!running_.load(std::memory_order_acquire)) return;

    std::lock_guard<std::mutex> lk(start_mu_);
    if (!running_.load(std::memory_order_relaxed)) return;

    {
        std::lock_guard<std::mutex> qlk(mu_);
        stopping_.store(true, std::memory_order_release);
    }
    cv_.notify_all();

    for (auto& t : threads_) t.join();
    threads_.clear();
    queue_.clear();
    running_.store(false, std::memory_order_release);
}

void BlockingPool::submit(Imp* imp, std::function<void()> fn) {
    ensure_started();
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back({imp, std::move(fn)});
    }
    cv_.notify_one();
}

void BlockingPool::worker() {
    for (;;) {
        Work w;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] {
                return !queue_.empty() ||
                       stopping_.load(std::memory_order_acquire);
            });
            if (queue_.empty()) return;  // stopping
            w = std::move(queue_.back());
            queue_.pop_back();
        }
        w.fn();
        w.imp->make_runnable();
    }
}

} // namespace csp::detail

// Public suspend primitive — follows the reactor's suspension pattern.
namespace csp::internal {

void run_blocking(std::function<void()> fn) {
    // Announce the suspend window BEFORE submit: the pool thread's
    // make_runnable must observe SUSP_PENDING (or later) so the wake
    // defers to CheckWP/drain. TLA:DrainSuspended.BeginSuspend
    detail::current_imp()->suspend_state_.store(
        detail::Imp::SUSP_PENDING, std::memory_order_release);
    detail::BlockingPool::instance().submit(detail::current_imp(), std::move(fn));
    detail::do_switch(detail::Status::detach);
}

} // namespace csp::internal
