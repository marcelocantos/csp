#pragma once

#include <csp/csp.h>
#include <csp/timer.h>

#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace csp {

struct restart_policy {
    int max_restarts = 3;
    csp::duration window = std::chrono::seconds(5);
    csp::duration backoff = csp::duration::zero();
};

struct max_restarts_exceeded : csp::error {
    std::string worker_name;
    std::exception_ptr cause;

    max_restarts_exceeded(std::string name, std::exception_ptr ex)
        : csp::error(
              "worker_group: max restarts exceeded for worker '" + name + "'")
        , worker_name(std::move(name))
        , cause(std::move(ex)) {}
};

class worker_group {
public:
    std::unordered_map<std::string, std::function<void()>> workers;
    restart_policy policy;

    void operator()() {
        if (workers.empty()) return;

        // Snapshot into a vector for indexed alt access.
        struct entry {
            std::string const* name;
            std::function<void()> const* factory;
        };
        std::vector<entry> specs;
        specs.reserve(workers.size());
        for (auto& [name, factory] : workers) {
            specs.push_back({&name, &factory});
        }

        size_t n = specs.size();
        std::vector<reader<std::exception_ptr>> handles(n);
        std::vector<bool> done(n, false);
        std::vector<std::deque<time_point>> restart_times(n);
        size_t active = n;

        for (size_t i = 0; i < n; ++i) {
            handles[i] = spawn(std::function<void()>(*specs[i].factory));
        }

        while (active > 0) {
            std::vector<chan_op<std::exception_ptr>> ops;
            ops.reserve(n);
            std::exception_ptr ep;
            for (size_t i = 0; i < n; ++i) {
                if (done[i]) {
                    ops.emplace_back();
                } else {
                    ops.push_back(handles[i] >> ep);
                }
            }

            int result = alt(ops);

            if (result >= 0) {
                auto idx = static_cast<size_t>(result);
                auto tp = now();
                auto& times = restart_times[idx];
                while (!times.empty() &&
                       times.front() + policy.window < tp) {
                    times.pop_front();
                }

                if (static_cast<int>(times.size()) >= policy.max_restarts) {
                    throw max_restarts_exceeded(*specs[idx].name, ep);
                }

                times.push_back(tp);

                if (policy.backoff > csp::duration::zero()) {
                    sleep(policy.backoff);
                }

                handles[idx] =
                    spawn(std::function<void()>(*specs[idx].factory));
            } else {
                auto idx = static_cast<size_t>(~result);
                done[idx] = true;
                --active;
            }
        }
    }

    void run() { (*this)(); }
};

} // namespace csp
