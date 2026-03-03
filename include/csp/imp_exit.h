#pragma once

#include <csp/csp.h>
#include <csp/timer.h>

#include <exception>
#include <functional>
#include <memory>

namespace csp {

struct restart_policy {
    int max_restarts = 3;
    duration window = std::chrono::seconds(5);
    duration backoff = duration::zero();
};

struct max_restarts_exceeded : csp::error {
    std::exception_ptr cause;
    max_restarts_exceeded(std::exception_ptr ex)
        : csp::error("max restarts exceeded"), cause(std::move(ex)) {}
};

struct imp_event {
    std::exception_ptr error;

    void restart(duration d = duration::zero());

    imp_event() = default;
    imp_event(imp_event&&) noexcept;
    imp_event& operator=(imp_event&&) noexcept;
    ~imp_event();

    imp_event(const imp_event&) = delete;
    imp_event& operator=(const imp_event&) = delete;

private:
    writer<duration> response_;
    imp_event(std::exception_ptr, writer<duration>);
    friend class supervised_fn;
};

// Callable wrapper returned by supervised(). Contains the retry loop
// in operator() (defined in imp_exit.cc, behind compilation firewall).
// Used as: spawn(supervised(f))
class supervised_fn {
    std::function<void()> f_;
public:
    explicit supervised_fn(std::function<void()> f);
    void operator()();
};

// Wrap a callable in a supervision retry loop.
// The returned supervised_fn checks the imp_exit dynamic on each exit.
// Move-only callables are supported via shared_ptr indirection.
template <typename F>
supervised_fn supervised(F&& f) {
    auto p = std::make_shared<std::decay_t<F>>(std::forward<F>(f));
    return supervised_fn([p = std::move(p)]() { (*p)(); });
}

class exit_guard {
public:
    exit_guard(exit_guard&&) noexcept;
    exit_guard& operator=(exit_guard&&) noexcept;
    ~exit_guard();

    exit_guard(const exit_guard&) = delete;
    exit_guard& operator=(const exit_guard&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
    friend exit_guard on_exit(std::function<void(imp_event)>);
    friend exit_guard on_exit(restart_policy);
    explicit exit_guard(std::unique_ptr<impl>);
};

exit_guard on_exit(std::function<void(imp_event)> handler);
exit_guard on_exit(restart_policy policy);

} // namespace csp
