#include <csp/timer.h>
#include <csp/cancel.h>
#include <csp/internal/signal.h>
#include <csp/internal/reactor.h>

#include <chrono>

namespace csp::internal {

void sleep_until(int64_t deadline_ns) {
    using namespace std::chrono;
    auto now_ns = steady_clock::now().time_since_epoch().count();
    auto delay_ns = deadline_ns - now_ns;
    if (delay_ns <= 0) return;

    auto signal = detail::create_timer_signal(delay_ns);
    csp::prialt(~signal);
}

} // namespace csp::internal

namespace csp {

void sleep_until(time_point tp) {
    if (is_cancel_active()) {
        // Cancel-aware path.
        if (!(*csp::clock)->uses_reactor()) {
            // Fake clock: spawn helper imp that does the raw sleep,
            // then race its death signal against cancel.
            chan<> timer;
            spawn([tp, w = std::move(timer.w)]() mutable {
                // Call clock virtual directly to avoid cancel recursion.
                (*csp::clock)->sleep_until(tp);
                // w drops on exit → timer.r sees death
            });
            auto cop = done();
            switch (prialt(std::move(cop), ~timer.r)) {
            case ~0: {
                auto reason = cancel_reason();
                if (reason) std::rethrow_exception(reason);
                throw canceled{};
            }
            case ~1: return;
            }
            return;
        }
        // Real clock: reactor timer signal + cancel.
        using namespace std::chrono;
        auto d = tp - steady_clock::now();
        auto delay_ns = duration_cast<nanoseconds>(d).count();
        if (delay_ns <= 0) return;  // already past

        auto signal = detail::create_timer_signal(delay_ns);
        auto cop = done();
        switch (prialt(std::move(cop), ~signal)) {
        case ~0: {
            auto reason = cancel_reason();
            if (reason) std::rethrow_exception(reason);
            throw canceled{};
        }
        case ~1: return;
        }
        return;
    }

    // No cancel scope: delegate to clock virtual.
    (*csp::clock)->sleep_until(tp);
}

void sleep(duration d) {
    sleep_until(csp::now() + d);
}

reader<time_point> after(duration d) {
    if (!(*csp::clock)->uses_reactor()) {
        // Fake clock: use the simple sleep+write path.
        // Use clock virtual directly to avoid cancel interference.
        return spawn_producer<time_point>([d](writer<time_point> w) {
            (*csp::clock)->sleep_until(csp::now() + d);
            w << csp::now();
        });
    }

    // Real clock: use reactor timer signal.
    // When the reader drops, ~w fires in prialt, the producer exits,
    // and signal's destructor cancels the EVFILT_TIMER. No leak.
    return spawn_producer<time_point>([d](writer<time_point> w) {
        using namespace std::chrono;
        auto delay_ns = duration_cast<nanoseconds>(d).count();
        if (delay_ns <= 0) delay_ns = 1;  // minimum 1ns
        auto signal = detail::create_timer_signal(delay_ns);
        switch (csp::prialt(~w, ~signal)) {
        case ~0: break;        // reader dropped — signal dtor cancels timer
        case ~1:
            w << csp::now();   // timer fired
            break;
        }
    });
}

reader<time_point> tick(duration interval) {
    return spawn_producer<time_point>([interval](writer<time_point> w) {
        auto next = csp::now() + interval;
        while (true) {
            csp::sleep_until(next);
            if (!(w << csp::now())) break;
            next += interval;
        }
    });
}

} // namespace csp
