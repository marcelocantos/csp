#pragma once

#include <doctest/doctest.h>

#include "csp.h"

#include <atomic>
#include <exception>
#include <mutex>

class RunScope;

class RunStats {
public:
    RunStats() {
        csp::global_exception_handler = csp::spawn_daemon_consumer<std::exception_ptr>([](auto && r) {
            for (std::exception_ptr ex; r >> ex;) {
                CHECK_NOTHROW(std::rethrow_exception(ex));
            }
        });
    }

    ~RunStats() {
        CHECK_EQ(0UL, pending());
        CHECK_EQ(0UL, running());

        csp::global_exception_handler = std::move(csp::chan<std::exception_ptr>().w);

        csp::internal::await_idle();

        if (!running()) {
            for (auto & ex : exs_) {
                std::exception_ptr ep;
                if (ex >> ep) {
                    try {
                        std::rethrow_exception(ep);
                    } catch (std::exception const & e) {
                        FAIL_CHECK("Imp threw exception: " << e.what());
                    } catch (...) {
                        FAIL_CHECK("Imp threw exception");
                    }
                }
            }
        }

        CHECK_EQ(0, csp::internal::channel_count(0));
        CHECK_EQ(0, csp::internal::channel_count(1));
    }

    size_t pending() { return pending_.load(); }
    size_t started() { return started_.load(); }
    size_t running() { return started_.load() - finished_.load(); }

    template <typename F>
    void spawn(F && f);

private:
    std::atomic<size_t> pending_{0};
    std::atomic<size_t> started_{0};
    std::atomic<size_t> finished_{0};
    std::vector<csp::reader<std::exception_ptr>> exs_;

    friend class RunScope;
};

class RunScope {
public:
    RunScope(RunStats & stats) : stats_(stats) {
        stats_.pending_.fetch_sub(1, std::memory_order_relaxed);
        stats_.started_.fetch_add(1, std::memory_order_relaxed);
    }
    ~RunScope() {
        stats_.finished_.fetch_add(1, std::memory_order_relaxed);
    }

private:
    RunStats & stats_;
};

template <typename F>
void RunStats::spawn(F && f) {
    pending_.fetch_add(1, std::memory_order_relaxed);
    csp::spawn([f = std::move(f), this]() mutable {
        RunScope scope(*this);
        f();
    });
}
