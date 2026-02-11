#ifndef INCLUDED__csp__test__testutil_h
#define INCLUDED__csp__test__testutil_h

#include <doctest/doctest.h>

#include <csp/microthread.h>

#include <exception>
#include <mutex>

// microthread.cc
int csp__internal__channel_count(int endpt);

class RunScope;

class RunStats {
public:
    RunStats() {
        csp::global_exception_handler = csp::spawn_consumer<std::exception_ptr>([](auto && r) {
            for (std::exception_ptr ex; r >> ex;) {
                CHECK_NOTHROW(std::rethrow_exception(ex));
            }
        });
    }

    ~RunStats() {
        CHECK_EQ(0UL, pending());
        CHECK_EQ(0UL, running());

        csp::global_exception_handler = ++csp::channel<std::exception_ptr>{};

        while (csp_run()) { }

        // TODO: Use alt to drain as many as possible.
        if (!running()) {
            for (auto & ex : exs_) {
                std::exception_ptr ep;
                if (ex >> ep) {
                    try {
                        std::rethrow_exception(ep);
                    } catch (std::exception const & e) {
                        FAIL_CHECK("Microthread threw exception: " << e.what());
                    } catch (...) {
                        FAIL_CHECK("Microthread threw exception");
                    }
                }
            }
        }

        CHECK_EQ(0, csp__internal__channel_count(0));
        CHECK_EQ(0, csp__internal__channel_count(1));
    }

    size_t pending() { return pending_; }
    size_t started() { return started_; }
    size_t running() { return started_ - finished_; }

    template <typename F>
    void spawn(F && f);

private:
    size_t pending_ = 0;
    size_t started_ = 0;
    size_t finished_ = 0;
    std::vector<csp::reader<std::exception_ptr>> exs_;

    friend class RunScope;
};

class RunScope {
public:
    RunScope(RunStats & stats) : stats_(stats) {
        --stats_.pending_;
        ++stats_.started_;
    }
    ~RunScope() {
        ++stats_.finished_;
    }

private:
    RunStats & stats_;
};

template <typename F>
void RunStats::spawn(F && f) {
    ++pending_;
    csp::spawn([f = std::move(f), this]{
        RunScope scope(*this);
        f();
    });
}

#endif // INCLUDED__csp__test__testutil_h
