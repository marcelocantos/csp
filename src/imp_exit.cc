#include <csp/imp_exit.h>
#include <csp/dynamic.h>

#include <deque>

namespace csp {

struct imp_exit_state {
    writer<imp_event> sink;
};

static dynamic<std::shared_ptr<imp_exit_state>> g_imp_exit{};

// --- imp_event ---

imp_event::imp_event(std::exception_ptr ex, writer<duration> w)
    : error(std::move(ex)), response_(std::move(w)) {}

imp_event::imp_event(imp_event&&) noexcept = default;
imp_event& imp_event::operator=(imp_event&&) noexcept = default;
imp_event::~imp_event() = default;

void imp_event::restart(duration d) {
    response_ << d;
    response_ = {};  // release writer after successful send
}

// --- supervised_fn ---

supervised_fn::supervised_fn(std::function<void()> f) : f_(std::move(f)) {}

void supervised_fn::operator()() {
    for (;;) {
        std::exception_ptr ex;
        try {
            f_();
        } catch (...) {
            ex = std::current_exception();
        }

        auto state = *g_imp_exit;
        if (!state) {
            if (ex) std::rethrow_exception(ex);
            return;
        }

        // Send event and wait for decision.
        // Copy ex (not move) — we need it if the send fails.
        chan<duration> response;
        imp_event event(ex, std::move(response.w));

        if (!(state->sink << std::move(event))) {
            // Policy channel dead — fall back to fail-fast.
            if (ex) std::rethrow_exception(ex);
            return;
        }

        duration d;
        if (!(response.r >> d)) {
            // Handler dropped event without restart() — die normally.
            // The handler already saw the exception and decided not to restart.
            return;
        }

        if (d > duration::zero()) csp::sleep(d);
    }
}

// --- exit_guard ---

struct exit_guard::impl {
    std::shared_ptr<imp_exit_state> state;
    csp::local binding;

    impl(std::shared_ptr<imp_exit_state> s)
        : state(std::move(s))
        , binding(g_imp_exit = state) {}
};

exit_guard::exit_guard(std::unique_ptr<impl> p) : impl_(std::move(p)) {}
exit_guard::exit_guard(exit_guard&&) noexcept = default;
exit_guard& exit_guard::operator=(exit_guard&&) noexcept = default;
exit_guard::~exit_guard() = default;

// --- on_exit ---

exit_guard on_exit(std::function<void(imp_event)> handler) {
    auto state = std::make_shared<imp_exit_state>();
    chan<imp_event> ch;
    state->sink = std::move(ch.w);

    spawn([handler = std::move(handler), r = std::move(ch.r)]() {
        for (;;) {
            imp_event ev;
            if (!(r >> ev)) break;
            handler(std::move(ev));
        }
    });

    return exit_guard(std::make_unique<exit_guard::impl>(state));
}

exit_guard on_exit(restart_policy policy) {
    auto state = std::make_shared<imp_exit_state>();
    chan<imp_event> ch;
    state->sink = std::move(ch.w);

    spawn([policy, r = std::move(ch.r)]() {
        std::deque<time_point> times;
        std::exception_ptr last_error;
        for (;;) {
            imp_event ev;
            if (!(r >> ev)) break;

            if (!ev.error) continue;  // ev destroyed at block end → imp dies

            auto tp = csp::now();
            while (!times.empty() && times.front() + policy.window < tp)
                times.pop_front();

            if (static_cast<int>(times.size()) >= policy.max_restarts) {
                break;  // ev destroyed at block end → imp dies with original exception
            }

            times.push_back(tp);
            ev.restart(policy.backoff);
        }
    });

    return exit_guard(std::make_unique<exit_guard::impl>(state));
}

} // namespace csp
