#include <csp/timer.h>
#include <csp/internal/csp_internal.h>

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

void fake_clock::sleep_until(time_point tp) {
    if (tp <= current_) return;
    pending_.push({tp, detail::current_imp()});
    internal::suspend();
}

void fake_clock::fire_expired() {
    while (!pending_.empty() && pending_.top().deadline <= current_) {
        auto* imp = pending_.top().imp;
        pending_.pop();
        // Use schedule() (global queue) rather than schedule_local() so that
        // workers parked after quiescence detection are woken to process the imp.
        imp->schedule();
    }
}

void fake_clock::advance(duration d) {
    current_ += d;
    fire_expired();
}

bool fake_clock::advance_to_next() {
    if (pending_.empty()) return false;
    current_ = pending_.top().deadline;
    fire_expired();
    return true;
}

void fake_clock::run() {
    for (;;) {
        csp::internal::run();  // drain until quiescent (respects sleeping imps)
        if (!advance_to_next()) break;
    }
}

void fake_clock::run_until_idle() {
    csp::internal::run();  // drain until quiescent (respects sleeping imps)
}

}
