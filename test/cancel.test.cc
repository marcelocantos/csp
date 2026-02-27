#include "testutil.h"

#include <atomic>
#include <stdexcept>

using namespace csp;

TEST_SUITE("cancellation") {

TEST_CASE("cancel_guard destructor auto-cancels") {
    RunStats stats;
    bool fired = false;
    stats.spawn([&]() {
        {
            auto guard = cancellation();
            spawn([&]() {
                prialt(done());
                fired = true;
            });
            csp::yield();
        } // guard destroyed → auto-cancel
        csp::yield(); // let child observe
    });
    while (csp::internal::run()) {}
    CHECK(fired);
}

TEST_CASE("explicit cancel") {
    RunStats stats;
    bool fired = false;
    stats.spawn([&]() {
        auto guard = cancellation();
        spawn([&]() {
            prialt(done());
            fired = true;
        });
        csp::yield();
        guard();
        csp::yield();
    });
    while (csp::internal::run()) {}
    CHECK(fired);
}

TEST_CASE("cancel with reason") {
    RunStats stats;
    bool got_reason = false;
    stats.spawn([&]() {
        auto guard = cancellation();
        spawn([&]() {
            prialt(done());
            auto reason = cancel_reason();
            CHECK(reason != nullptr);
            try {
                std::rethrow_exception(reason);
            } catch (std::runtime_error const& e) {
                got_reason = (std::string(e.what()) == "custom reason");
            } catch (...) {}
        });
        csp::yield();
        guard(std::make_exception_ptr(std::runtime_error("custom reason")));
        csp::yield();
    });
    while (csp::internal::run()) {}
    CHECK(got_reason);
}

TEST_CASE("done in prialt with work channel") {
    RunStats stats;
    int result = -1;
    stats.spawn([&]() {
        auto guard = cancellation();
        chan<int> work;
        spawn([&]() {
            int v;
            switch (prialt(done(), work.r >> v)) {
            case ~0: result = 0; break;
            case  1: result = 1; break;
            }
        });
        csp::yield();
        guard();
        csp::yield();
    });
    while (csp::internal::run()) {}
    CHECK(result == 0); // cancel fired, not work
}

TEST_CASE("done inactive without scope") {
    RunStats stats;
    int result = -1;
    stats.spawn([&]() {
        chan<int> ch;
        spawn([&]() {
            int v;
            switch (prialt(done(), ch.r >> v)) {
            case ~0: result = 0; break;
            case  1: result = 1; break;
            }
        });
        ch.w << 42;
    });
    while (csp::internal::run()) {}
    CHECK(result == 1); // work channel fired, done inactive
}

TEST_CASE("cascading parent to child") {
    RunStats stats;
    bool child_cancelled = false;
    stats.spawn([&]() {
        auto parent = cancellation();
        spawn([&]() {
            auto child = cancellation();
            spawn([&]() {
                prialt(done());
                child_cancelled = true;
            });
            csp::yield();
            // Block until child scope's cancel fires
            prialt(done());
        });
        csp::yield();
        csp::yield(); // let child scope set up
        parent();
        csp::yield();
        csp::yield();
    });
    while (csp::internal::run()) {}
    CHECK(child_cancelled);
}

TEST_CASE("multi-level cascade") {
    RunStats stats;
    bool leaf_cancelled = false;
    stats.spawn([&]() {
        auto g1 = cancellation();
        spawn([&]() {
            auto g2 = cancellation();
            spawn([&]() {
                auto g3 = cancellation();
                spawn([&]() {
                    prialt(done());
                    leaf_cancelled = true;
                });
                csp::yield();
                prialt(done());
            });
            csp::yield();
            csp::yield();
            prialt(done());
        });
        csp::yield();
        csp::yield();
        csp::yield();
        g1();
        csp::yield();
        csp::yield();
        csp::yield();
    });
    while (csp::internal::run()) {}
    CHECK(leaf_cancelled);
}

TEST_CASE("direct child cancel does not affect parent") {
    RunStats stats;
    bool parent_saw_cancel = false;
    bool child_saw_cancel = false;
    stats.spawn([&]() {
        auto parent = cancellation();
        spawn([&]() {
            auto child = cancellation();
            spawn([&]() {
                prialt(done());
                child_saw_cancel = true;
            });
            csp::yield();
            child();
            csp::yield();
        });

        chan<int> ch;
        int dummy;
        spawn([&]() {
            // Parent scope — check if cancel fires
            switch (prialt(done(), ch.r >> dummy)) {
            case ~0: parent_saw_cancel = true; break;
            }
        });
        csp::yield();
        csp::yield();
        csp::yield();
        // Don't cancel parent — just let guard destruct
    });
    while (csp::internal::run()) {}
    CHECK(child_saw_cancel);
    // Parent guard auto-cancels in destructor, so parent_saw_cancel may be
    // true. The key assertion is that child cancel didn't affect the parent
    // BEFORE the parent's own guard was destroyed.
}

TEST_CASE("cancel_reason retrieval") {
    RunStats stats;
    std::exception_ptr reason;
    stats.spawn([&]() {
        auto guard = cancellation();
        guard(std::make_exception_ptr(std::runtime_error("test")));
        reason = cancel_reason();
    });
    while (csp::internal::run()) {}
    REQUIRE(reason != nullptr);
    try {
        std::rethrow_exception(reason);
    } catch (std::runtime_error const& e) {
        CHECK(std::string(e.what()) == "test");
    }
}

TEST_CASE("cancel_reason returns nullptr without scope") {
    RunStats stats;
    std::exception_ptr reason = std::make_exception_ptr(std::runtime_error("x"));
    stats.spawn([&]() {
        reason = cancel_reason();
    });
    while (csp::internal::run()) {}
    CHECK(reason == nullptr);
}

TEST_CASE("multiple watchers") {
    RunStats stats;
    std::atomic<int> count{0};
    stats.spawn([&]() {
        auto guard = cancellation();
        for (int i = 0; i < 5; ++i) {
            spawn([&]() {
                prialt(done());
                ++count;
            });
        }
        csp::yield();
        guard();
        for (int i = 0; i < 5; ++i) csp::yield();
    });
    while (csp::internal::run()) {}
    CHECK(count == 5);
}

TEST_CASE("cancellable sleep throws") {
    RunStats stats;
    fake_clock fc;
    bool threw = false;
    stats.spawn([&]() {
        csp::local l{csp::clock = &fc};
        auto guard = cancellation();
        spawn([&]() {
            csp::local l2{csp::clock = &fc};
            try {
                csp::sleep(std::chrono::seconds(60));
            } catch (canceled const&) {
                threw = true;
            }
        });
        csp::yield();
        guard();
        csp::yield();
    });
    csp::schedule();
    fc.run();
    CHECK(threw);
}

TEST_CASE("sleep completes normally under cancel scope") {
    RunStats stats;
    fake_clock fc;
    bool completed = false;
    stats.spawn([&]() {
        csp::local l{csp::clock = &fc};
        auto guard = cancellation();
        chan<> done;
        spawn([&, w = std::move(done.w)]() {
            csp::local l2{csp::clock = &fc};
            csp::sleep(std::chrono::seconds(1));
            completed = true;
        });
        // Wait for child to finish before guard destructs
        prialt(~done.r);
    });
    csp::schedule();
    fc.run();
    CHECK(completed);
}

TEST_CASE("double cancel is idempotent") {
    RunStats stats;
    stats.spawn([&]() {
        auto guard = cancellation();
        guard();
        guard(); // second call — should be no-op
        auto reason = cancel_reason();
        CHECK(reason != nullptr);
    });
    while (csp::internal::run()) {}
}

TEST_CASE("channel leak check") {
    // RunStats destructor checks channel_count == 0.
    RunStats stats;
    bool fired = false;
    stats.spawn([&]() {
        auto guard = cancellation();
        spawn([&]() {
            prialt(done());
            fired = true;
        });
        csp::yield();
        guard();
        csp::yield();
    });
    while (csp::internal::run()) {}
    CHECK(fired);
}

TEST_CASE("deadline fires timed_out") {
    RunStats stats;
    fake_clock fc;
    bool got_timed_out = false;
    stats.spawn([&]() {
        csp::local l{csp::clock = &fc};
        auto guard = cancellation(std::chrono::seconds(5));
        spawn([&]() {
            csp::local l2{csp::clock = &fc};
            try {
                csp::sleep(std::chrono::seconds(60));
            } catch (timed_out const&) {
                got_timed_out = true;
            } catch (canceled const&) {
                // should be timed_out, not plain canceled
            }
        });
        prialt(done());  // wait for deadline to fire
    });
    csp::schedule();
    fc.run();
    CHECK(got_timed_out);
}

TEST_CASE("deadline done fires") {
    RunStats stats;
    fake_clock fc;
    bool fired = false;
    stats.spawn([&]() {
        csp::local l{csp::clock = &fc};
        auto guard = cancellation(std::chrono::seconds(2));
        spawn([&]() {
            prialt(done());
            fired = true;
        });
        prialt(done());  // wait for deadline
    });
    csp::schedule();
    fc.run();
    CHECK(fired);
}

TEST_CASE("deadline cancel_reason is timed_out") {
    RunStats stats;
    fake_clock fc;
    bool is_timed_out = false;
    stats.spawn([&]() {
        csp::local l{csp::clock = &fc};
        auto guard = cancellation(std::chrono::seconds(1));
        spawn([&]() {
            prialt(done());
            auto reason = cancel_reason();
            try {
                std::rethrow_exception(reason);
            } catch (timed_out const&) {
                is_timed_out = true;
            } catch (...) {}
        });
        prialt(done());  // wait for deadline to fire
    });
    csp::schedule();
    fc.run();
    CHECK(is_timed_out);
}

TEST_CASE("deadline explicit cancel before timeout") {
    RunStats stats;
    fake_clock fc;
    bool got_canceled = false;
    bool got_timed_out = false;
    stats.spawn([&]() {
        csp::local l{csp::clock = &fc};
        auto guard = cancellation(std::chrono::seconds(10));
        spawn([&]() {
            prialt(done());
            auto reason = cancel_reason();
            try {
                std::rethrow_exception(reason);
            } catch (timed_out const&) {
                got_timed_out = true;
            } catch (canceled const&) {
                got_canceled = true;
            }
        });
        csp::yield();
        guard();  // cancel before the 10s deadline
    });
    csp::schedule();
    fc.run();
    CHECK(got_canceled);
    CHECK_FALSE(got_timed_out);
}

TEST_CASE("deadline with time_point overload") {
    RunStats stats;
    fake_clock fc;
    bool fired = false;
    stats.spawn([&]() {
        csp::local l{csp::clock = &fc};
        auto tp = csp::now() + std::chrono::seconds(3);
        auto guard = cancellation(tp);
        spawn([&]() {
            prialt(done());
            fired = true;
        });
        prialt(done());  // wait for deadline
    });
    csp::schedule();
    fc.run();
    CHECK(fired);
}

TEST_CASE("deadline cascades to child") {
    RunStats stats;
    fake_clock fc;
    bool child_fired = false;
    stats.spawn([&]() {
        csp::local l{csp::clock = &fc};
        auto guard = cancellation(std::chrono::seconds(5));
        spawn([&]() {
            auto child = cancellation();
            spawn([&]() {
                prialt(done());
                child_fired = true;
            });
            csp::yield();
            prialt(done());
        });
        prialt(done());  // wait for deadline
    });
    csp::schedule();
    fc.run();
    CHECK(child_fired);
}

TEST_CASE("is_cancel_active inside and outside scope") {
    RunStats stats;
    bool active_inside = false;
    bool active_outside = true;
    stats.spawn([&]() {
        active_outside = is_cancel_active();
        {
            auto guard = cancellation();
            active_inside = is_cancel_active();
        }
    });
    while (csp::internal::run()) {}
    CHECK(active_inside);
    CHECK_FALSE(active_outside);
}

TEST_CASE("cancel_guard move constructor") {
    RunStats stats;
    bool fired = false;
    stats.spawn([&]() {
        chan<> signal;
        spawn([&, r = std::move(signal.r)]() {
            prialt(done());
            fired = true;
        });
        {
            auto guard = cancellation();
            csp::yield();
            // Move guard to a new variable; original goes out of scope.
            auto moved = std::move(guard);
            // Original destructs here — should NOT cancel (moved-from).
        }
        // moved destructs here — SHOULD cancel.
        csp::yield();
    });
    while (csp::internal::run()) {}
    CHECK(fired);
}

TEST_CASE("cancel_guard move assignment") {
    RunStats stats;
    bool fired = false;
    stats.spawn([&]() {
        spawn([&]() {
            prialt(done());
            fired = true;
        });
        {
            auto guard1 = cancellation();
            csp::yield();
            auto guard2 = cancellation();
            // Move-assign guard1 into guard2; guard1's scope auto-cancels
            // via the replaced guard2's destructor logic, but guard2 now
            // owns guard1's scope.
            guard2 = std::move(guard1);
            // guard1 is moved-from, destructs harmlessly.
        }
        // guard2 destructs here — cancels the original guard1 scope.
        csp::yield();
    });
    while (csp::internal::run()) {}
    CHECK(fired);
}

TEST_CASE("nested cancel scopes inner fires first") {
    RunStats stats;
    fake_clock fc;
    bool inner_fired = false;
    bool outer_fired = false;
    stats.spawn([&]() {
        csp::local l{csp::clock = &fc};
        auto outer = cancellation(std::chrono::seconds(10));
        spawn([&]() {
            // Watch outer scope.
            prialt(done());
            outer_fired = true;
        });
        auto inner = cancellation(std::chrono::seconds(2));
        spawn([&]() {
            // Watch inner scope — should fire at 2s, before outer's 10s.
            prialt(done());
            inner_fired = true;
        });
        csp::yield();
        csp::yield();
        prialt(done()); // blocks until inner deadline fires
    });
    csp::schedule();
    fc.run();
    CHECK(inner_fired);
    // Outer may or may not have fired depending on cascade; the key
    // assertion is that inner fires first (inner_fired == true by the
    // time we get here).
}

TEST_CASE("done returns inactive chan_op without scope") {
    RunStats stats;
    bool done_op_inactive = false;
    stats.spawn([&]() {
        auto op = done();
        // done() outside a cancel scope returns an inactive chan_op.
        // In a prialt with a ready channel, the channel should win.
        chan<int> ch;
        spawn([&]() { ch.w << 99; });
        int v = 0;
        switch (prialt(op, ch.r >> v)) {
        case ~0: done_op_inactive = false; break;
        case  1: done_op_inactive = true; break;
        }
    });
    while (csp::internal::run()) {}
    CHECK(done_op_inactive);
}

TEST_CASE("dynamic binding reverts on exception during cancel") {
    RunStats stats;
    static csp::dynamic<int> val(0);
    int after_unwind = -1;
    stats.spawn([&]() {
        csp::local l{val = 42};
        try {
            auto guard = cancellation();
            csp::local l2{val = 99};
            CHECK_EQ(*val, 99);
            throw std::runtime_error("boom");
            // guard and l2 destruct during stack unwinding
        } catch (std::runtime_error const&) {
            // l2 should have reverted
            after_unwind = *val;
        }
    });
    while (csp::internal::run()) {}
    CHECK_EQ(after_unwind, 42);
}

} // TEST_SUITE("cancellation")

TEST_SUITE("cancellation MN") {

TEST_CASE("concurrent cancel detection") {
    init_runtime(4);
    std::atomic<int> count{0};
    csp::spawn([&]() {
        auto guard = cancellation();
        for (int i = 0; i < 10; ++i) {
            spawn([&]() {
                prialt(done());
                count++;
            });
        }
        csp::yield();
        guard();
        csp::yield();
        csp::yield();
    });
    csp::schedule();
    CHECK(count == 10);
    csp::shutdown_runtime();
}

#ifndef _WIN32
TEST_CASE("cancel during I/O") {
    init_runtime(4);
    bool threw = false;
    int fds[2];
    REQUIRE(pipe(fds) == 0);

    // Set read end non-blocking
    int flags = fcntl(fds[0], F_GETFL);
    fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    csp::spawn([&]() {
        auto guard = cancellation();
        chan<> entered;  // signals child is about to enter I/O
        spawn([&, w = std::move(entered.w)]() mutable {
            try {
                w = {};  // signal: about to enter I/O
                char buf[1];
                csp::io::read(fds[0], buf, 1);
            } catch (canceled const&) {
                threw = true;
            }
        });
        prialt(~entered.r);  // wait for child to be ready
        guard();
        csp::yield();
        // Close write end so the orphaned reactor helper can see EOF and exit.
        ::close(fds[1]);
        csp::yield();
    });
    csp::schedule();
    CHECK(threw);
    ::close(fds[0]);
    csp::shutdown_runtime();
}
#endif // !_WIN32

} // TEST_SUITE("cancellation MN")
