#pragma once

#include <csp/csp.h>
#include <csp/timer.h>

#include <exception>
#include <memory>

namespace csp {

struct canceled : csp::error {
    canceled() : error("canceled") {}
protected:
    canceled(const char* msg) : error(msg) {}
};

struct timed_out : canceled {
    timed_out() : canceled("timed out") {}
};

class cancel_guard {
public:
    cancel_guard(cancel_guard&&) noexcept;
    cancel_guard& operator=(cancel_guard&&) noexcept;
    ~cancel_guard();

    void operator()();
    void operator()(std::exception_ptr);

    cancel_guard(cancel_guard const&) = delete;
    cancel_guard& operator=(cancel_guard const&) = delete;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
    friend cancel_guard cancellation();
    friend cancel_guard cancellation(time_point);
    explicit cancel_guard(std::unique_ptr<impl>);
};

cancel_guard cancellation();
cancel_guard cancellation(duration d);
cancel_guard cancellation(time_point tp);
chan_op<> done();
bool is_cancel_active();
std::exception_ptr cancel_reason();

} // namespace csp
