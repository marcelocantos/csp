#include <csp/timer.h>
#include <csp/internal/csp_internal.h>

namespace csp {

dynamic<fake_clock*> clock{nullptr};

fake_clock::fake_clock(time_point start) : current_(start) {}

void fake_clock::sleep_until_impl(time_point tp) {
    if (tp <= current_) return;
    pending_.push({tp, detail::g_imp});
    internal::suspend();
}

void fake_clock::fire_expired() {
    while (!pending_.empty() && pending_.top().deadline <= current_) {
        auto* imp = pending_.top().imp;
        pending_.pop();
        imp->schedule_local();
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
        while (internal::run()) {}
        if (!advance_to_next()) break;
    }
}

void fake_clock::run_until_idle() {
    while (internal::run()) {}
}

}
