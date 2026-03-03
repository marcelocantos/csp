#pragma once

#include <csp/csp.h>
#include <csp/imp_exit.h>

#include <functional>
#include <string>
#include <unordered_map>

namespace csp {

// restart_policy and max_restarts_exceeded are defined in <csp/imp_exit.h>.

// worker_group-specific exception with worker name.
struct worker_max_restarts_exceeded : csp::error {
    std::string worker_name;
    std::exception_ptr cause;

    worker_max_restarts_exceeded(std::string name, std::exception_ptr ex)
        : csp::error(
              "worker_group: max restarts exceeded for worker '" + name + "'")
        , worker_name(std::move(name))
        , cause(std::move(ex)) {}
};

// Deprecated: prefer on_exit(restart_policy) + spawn(supervised(...)).
class worker_group {
public:
    std::unordered_map<std::string, std::function<void()>> workers;
    restart_policy policy;

    void operator()();
    void run() { (*this)(); }
};

} // namespace csp
