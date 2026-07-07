#include <csp/timer.h>
#include <csp/internal/csp_internal.h>
#include <csp/internal/runtime.h>

namespace csp {

namespace {

struct real_clock : clock_source {
    time_point now() const override {
        return std::chrono::steady_clock::now();
    }
    void sleep_until(time_point tp) override {
        internal::sleep_until(tp.time_since_epoch().count());
    }
};

real_clock real_clock_instance;

} // namespace

dynamic<std::shared_ptr<clock_source>> clock{
    std::shared_ptr<clock_source>(&real_clock_instance, [](auto*){})
};

fake_clock::fake_clock(time_point start) : current_(start) {}

time_point fake_clock::now() const {
    // Auto-enroll the calling imp in this clock's quiescence scope.
    // Only enroll real imps (with fcontext stacks), not p.main.
    auto* imp = detail::current_imp();
    if (imp && imp->stk_.base && imp->qs_ != &qs_ && !imp->qs_entered_) {
        imp->qs_ = const_cast<quiescence_scope*>(&qs_);
        const_cast<quiescence_scope*>(&qs_)->enter();
        imp->qs_entered_ = true;
    }
    return current_;
}

fake_clock::~fake_clock() {
    // Unregister quiescence hook.
    auto& rt = detail::Runtime::instance();
    std::lock_guard<std::mutex> lk(rt.hook_mu_);
    rt.quiescence_hook_ = nullptr;
    rt.has_hook_.store(false, std::memory_order_release);
}

void fake_clock::sleep_until(time_point tp) {
    if (tp <= current_) return;
    auto token = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push({tp, detail::current_imp(), token});
    }
    // Register quiescence hook on first use (lazy — avoids needing
    // the runtime to be initialized at fake_clock construction time).
    auto& rt = detail::Runtime::instance();
    {
        std::lock_guard<std::mutex> lk(rt.hook_mu_);
        if (!rt.quiescence_hook_) {
            rt.quiescence_hook_ = [this]() -> bool {
                // Only advance time when ALL scope members are sleeping.
                if (!qs_.is_quiescent()) return true;  // imps still active
                return advance_to_next();  // true=advanced, false=no timers
            };
            rt.has_hook_.store(true, std::memory_order_release);
        }
    }
    internal::suspend();
    // Imp woke (timer fired or cancelled). Mark the timer entry
    // so fire_expired skips it if it's still in pending_.
    token->store(true, std::memory_order_release);
}

void fake_clock::fire_expired() {
    std::vector<detail::Imp*> expired;
    {
        std::lock_guard<std::mutex> lk(mu_);
        while (!pending_.empty() && pending_.top().deadline <= current_) {
            auto& e = pending_.top();
            if (!e.cancelled->load(std::memory_order_acquire)) {
                expired.push_back(e.imp);
            }
            pending_.pop();
        }
    }
    for (auto* imp : expired) imp->make_runnable();
}

void fake_clock::advance(duration d) {
    // Write current_ under mu_: advance_to_next() (the quiescence hook,
    // possibly on another thread) writes it under mu_ concurrently, and a
    // torn/lost advance would strand a sleeper whose deadline is never
    // reached. The unlocked *read* in now() remains covered by the TSan
    // suppression (see test/tsan_suppressions.txt).
    {
        std::lock_guard<std::mutex> lk(mu_);
        current_ += d;
    }
    fire_expired();
}

bool fake_clock::advance_to_next() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pending_.empty()) return false;
        current_ = pending_.top().deadline;
    }
    fire_expired();
    return true;
}

dynamic_binding fake_clock::binding() {
    // Set qs_ for child propagation WITHOUT entering the scope.
    // The binding imp is the orchestrator, not a participant —
    // only child imps (spawned after this) enter the scope.
    detail::current_imp()->qs_ = &qs_;
    return csp::clock = std::shared_ptr<clock_source>(this, [](auto*){});
}

void fake_clock::run() {
    qs_.bind();
    for (;;) {
        qs_.wait();
        if (!advance_to_next()) break;
    }
}

void fake_clock::run_until_idle() {
    qs_.bind();
    qs_.wait();
}

}
