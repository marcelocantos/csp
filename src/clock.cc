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

dynamic<clock_source*> clock{&real_clock_instance};

fake_clock::fake_clock(time_point start) : current_(start) {}

fake_clock::~fake_clock() {
    // Unregister quiescence hook.
    auto& rt = detail::Runtime::instance();
    std::lock_guard<std::mutex> lk(rt.hook_mu_);
    rt.quiescence_hook_ = nullptr;
}

void fake_clock::sleep_until(time_point tp) {
    if (tp <= current_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_.push({tp, detail::current_imp()});
    }
    // Register quiescence hook on first use (lazy — avoids needing
    // the runtime to be initialized at fake_clock construction time).
    auto& rt = detail::Runtime::instance();
    {
        std::lock_guard<std::mutex> lk(rt.hook_mu_);
        if (!rt.quiescence_hook_) {
            rt.quiescence_hook_ = [this]{ return advance_to_next(); };
        }
    }
    internal::suspend();
}

void fake_clock::fire_expired() {
    std::vector<detail::Imp*> expired;
    {
        std::lock_guard<std::mutex> lk(mu_);
        while (!pending_.empty() && pending_.top().deadline <= current_) {
            expired.push_back(pending_.top().imp);
            pending_.pop();
        }
    }
    for (auto* imp : expired) imp->schedule();
}

void fake_clock::advance(duration d) {
    current_ += d;
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
