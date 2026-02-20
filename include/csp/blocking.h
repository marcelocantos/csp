#pragma once

#include <csp/csp.h>

#include <type_traits>
#include <utility>

namespace csp::internal {

// Suspend the calling imp and run fn on the blocking pool.
// Resumes the imp when fn returns. Declared in csp.h's
// internal namespace; defined in blocking_pool.cc.
void run_blocking(std::function<void()> fn);

}

namespace csp {

// Run fn on an OS thread pool, suspending the calling imp until
// it completes. Use this for blocking OS calls (DNS, file I/O on
// non-fd-based APIs, third-party libraries) that would otherwise stall
// the processor.
//
// Returns the value returned by fn.
//
// Example:
//   auto addrs = csp::blocking([]{ return getaddrinfo(...); });
//
template <typename Fn>
auto blocking(Fn&& fn) -> std::invoke_result_t<Fn> {
    using R = std::invoke_result_t<Fn>;
    if constexpr (std::is_void_v<R>) {
        internal::run_blocking(std::forward<Fn>(fn));
    } else {
        R result;
        internal::run_blocking([&] { result = fn(); });
        return result;
    }
}

}
