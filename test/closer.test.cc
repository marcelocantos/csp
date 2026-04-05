#include "testutil.h"

using namespace csp;

TEST_SUITE("Closer") {

TEST_CASE("closer-wraps-reader-for-death-watch") {
    RunStats stats;

    csp::run([&] {
        chan<int> ch;

        // Wrap the reader in a closer.
        closer c(std::move(ch.r));

        // closer is truthy while the channel is alive.
        CHECK(bool(c));

        // Death-watch via ~closer works in prialt.
        spawn([w = std::move(ch.w)] {
            w << 42;
        });

        // The closer can't read (no operator>>), only death-watch.
        // When the writer dies after sending, the reader (inside closer)
        // has unread data — but closer can't access it.
        // Eventually both endpoints die.
    });
}

TEST_CASE("closer-wraps-writer-for-death-watch") {
    RunStats stats;

    csp::run([&] {
        chan<int> ch;

        closer c(std::move(ch.w));
        CHECK(bool(c));

        // Drop the reader — writer (inside closer) sees death.
        ch.r = {};

        // closer should now be dead.
        // Use in prialt:
        auto k = prialt(~c);
        CHECK(k == ~0);
    });
}

TEST_CASE("closer-from-spawn-handle") {
    RunStats stats;

    csp::run([&] {
        // spawn returns reader<exception_ptr>; wrap in closer for
        // lifecycle-only observation.
        closer handle(spawn([] {
            // do nothing
        }));

        CHECK(bool(handle));

        // Death-watch: imp finishes → handle fires.
        auto k = prialt(~handle);
        CHECK(k == ~0);
    });
}

TEST_CASE("dropping-closer-signals-other-side") {
    RunStats stats;

    bool writer_saw_death = false;

    csp::run([&] {
        chan<int> ch;

        spawn([w = std::move(ch.w), &writer_saw_death] {
            int v = 42;
            auto k = prialt(w << std::move(v));
            if (k == ~0) writer_saw_death = true;
        });

        {
            closer c(std::move(ch.r));
            // c goes out of scope here — drops the reader.
        }
    });

    CHECK(writer_saw_death);
}

TEST_CASE("closer-endpoint()-accessor") {
    RunStats stats;

    csp::run([&] {
        chan<int> ch;
        closer c(std::move(ch.r));

        // endpoint() gives access to the underlying reader.
        CHECK(bool(c.endpoint()));

        // Can read via endpoint() if needed (escape hatch).
        spawn([w = std::move(ch.w)] {
            w << 99;
        });

        int v;
        c.endpoint() >> v;
        CHECK(v == 99);
    });
}

} // TEST_SUITE
