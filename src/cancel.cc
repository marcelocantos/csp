#include <csp/cancel.h>
#include <csp/dynamic.h>
#include <csp/timer.h>
#include <csp/internal/signal.h>

#include <atomic>
#include <chrono>
#include <memory>

namespace csp {

struct cancel_state {
    writer<> trigger;
    reader<> signal;
    std::exception_ptr reason;
    std::atomic<bool> cancelled{false};

    cancel_state() {
        chan<> ch;
        trigger = std::move(ch.w);
        signal = std::move(ch.r);
    }

    void cancel(std::exception_ptr ep) {
        if (!cancelled.exchange(true)) {
            reason = ep;
            trigger = {};  // drop writer → fires all vultures on signal
        }
    }
};

// Stored as shared_ptr in the HAMT so that child imps (via inherited
// dynamic scope) keep the cancel_state alive even after the guard that
// created it is destroyed.
static dynamic<std::shared_ptr<cancel_state>> g_cancel{};

// --- cancel_guard ---

struct cancel_guard::impl {
    std::shared_ptr<cancel_state> state;
    csp::local binding;

    impl(std::shared_ptr<cancel_state> s)
        : state(std::move(s))
        , binding(g_cancel = state)
    {}
};

cancel_guard::cancel_guard(std::unique_ptr<impl> p) : impl_(std::move(p)) {}
cancel_guard::cancel_guard(cancel_guard&&) noexcept = default;
cancel_guard& cancel_guard::operator=(cancel_guard&&) noexcept = default;

cancel_guard::~cancel_guard() {
    if (impl_ && !impl_->state->cancelled)
        impl_->state->cancel(std::make_exception_ptr(canceled{}));
}

void cancel_guard::operator()() {
    impl_->state->cancel(std::make_exception_ptr(canceled{}));
}

void cancel_guard::operator()(std::exception_ptr ep) {
    impl_->state->cancel(std::move(ep));
}

// --- Public API ---

cancel_guard cancellation() {
    auto parent = *g_cancel;  // shared_ptr copy (may be null)
    auto state = std::make_shared<cancel_state>();

    if (parent) {
        // Cascade: spawn imp that watches parent and cancels child.
        // Captures shared_ptrs to keep both states alive.
        spawn([p = std::move(parent), c = state]() {
            switch (prialt(~p->signal, ~c->signal)) {
            case ~0:  // parent cancelled → cascade
                c->cancel(p->reason);
                break;
            case ~1:  // child cancelled directly → exit
                break;
            }
        });
    }

    return cancel_guard(std::make_unique<cancel_guard::impl>(state));
}

cancel_guard cancellation(duration d) {
    return cancellation(csp::now() + d);
}

cancel_guard cancellation(time_point tp) {
    auto parent = *g_cancel;
    auto state = std::make_shared<cancel_state>();

    if (parent) {
        spawn([p = std::move(parent), c = state]() {
            switch (prialt(~p->signal, ~c->signal)) {
            case ~0:  c->cancel(p->reason); break;
            case ~1:  break;
            }
        });
    }

    // Timer imp: use reactor timer signal directly, no after()+helper.
    // If scope is cancelled first, timer_signal dtor cancels the kqueue event.
    spawn([tp, c = state]() {
        if (!(*csp::clock)->uses_reactor()) {
            // Fake clock: use after() helper imp (no reactor involvement).
            auto timer = csp::after(tp - csp::now());
            switch (prialt(~c->signal, timer >> nullptr)) {
            case ~0: break;
            case  1:
                c->cancel(std::make_exception_ptr(timed_out{}));
                break;
            }
            return;
        }
        // Real clock: reactor timer signal, no helper imp.
        using namespace std::chrono;
        auto d = tp - steady_clock::now();
        auto delay_ns = duration_cast<nanoseconds>(d).count();
        if (delay_ns <= 0) {
            c->cancel(std::make_exception_ptr(timed_out{}));
            return;
        }
        auto signal = detail::create_timer_signal(delay_ns);
        switch (prialt(~c->signal, ~signal)) {
        case ~0: break;  // scope cancelled — timer_signal dtor cancels kqueue event
        case ~1:         // deadline fired
            c->cancel(std::make_exception_ptr(timed_out{}));
            break;
        }
    });

    return cancel_guard(std::make_unique<cancel_guard::impl>(state));
}

chan_op<> done() {
    auto state = *g_cancel;  // shared_ptr copy keeps state alive
    if (!state) return {};
    return ~state->signal;
}

bool is_cancel_active() {
    auto state = *g_cancel;
    return state != nullptr;
}

std::exception_ptr cancel_reason() {
    auto state = *g_cancel;
    if (!state || !state->cancelled) return {};
    return state->reason;
}

} // namespace csp
