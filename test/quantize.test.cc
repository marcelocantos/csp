#include "testutil.h"

using namespace csp;
using namespace csp::part;

static Logger g_log("Quantize.Test");

TEST_CASE("Quantize---Simple") {
    RunStats stats;

    csp::run([&]{
        writer<int> in, quanta;
        reader<int> out, residue;

        stats.spawn(quantize(--in, --quanta, ++out, ++residue));

        quanta << 5; quanta = {};
        in << 7; in = {};
        CHECK(5 == out.read());
        CHECK(2 == residue.read());
        // csp::run waits for the quantize imp to exit before returning.
    });
}

TEST_CASE("Quantize---Complex") {
    RunStats stats;

    int loops = 11;
    int sent = 0, delivered = 0, undelivered = 0;

    csp::run([&]{
        auto [source_w, source_r] = chan<int>{};
        stats.spawn([loops, w = std::move(source_w), &sent]{
            constexpr int delta = 23;
            for (int i = 0; i < loops * (7 + 13 + 11) && w << delta; i += delta) {
                CSP_LOG(g_log, "  \033[31mi=%3d δ=%d\033[0m", i, delta);
                sent += delta;
            }
        });

        std::vector<int> qdata = {7, 13, 11};

        auto [quanta_w, quanta_r] = chan<int>{};
        reader<int> residue;
        stats.spawn(enumerate(std::vector<int>{qdata}, true).bind(std::move(quanta_w)));

        reader<int> sink;
        stats.spawn(quantize(std::move(source_r), std::move(quanta_r), ++sink, ++residue));

        stats.spawn([loops, &qdata, &delivered, sink = std::move(sink)]{
            for (int i = 0; i < loops; ++i) {
                for (int n : qdata) {
                    int v;
                    if (!(sink >> v)) {
                        break;
                    }
                    delivered += v;
                    CSP_LOG(g_log, "i=%3d n=%d v=%d", i, n, v);
                    INFO("i = " << i);
                    CHECK(n == v);
                }
            }
            CHECK_FALSE((sink >> nullptr));
        });

        CHECK(bool(residue >> undelivered));
        // csp::run waits for all imps (source, enumerate, quantize, sink) to exit.
    });

    // All imps done — sent, delivered, undelivered are fully written.
    INFO(sent << " ≠ " << undelivered << " + " << delivered);
    CHECK(sent == undelivered + delivered);
}

TEST_CASE("Quantize---Uniform") {
    RunStats stats;

    int sent = 0, delivered = 0, undelivered = 0;

    csp::run([&]{
        auto [source_w, source_r] = chan<int>{};
        stats.spawn([w = std::move(source_w), &sent]{
            constexpr int delta = 23;
            for (int i = 0; i < 13 * 7 && w << delta; i += delta) {
                sent += delta;
            }
        });

        constexpr int quantum = 7;

        reader<int> sink;
        reader<int> residue;
        stats.spawn(quantize(std::move(source_r), quantum, ++sink, ++residue));

        stats.spawn([sink = std::move(sink), quantum, &delivered]{
            for (int n; sink >> n;) {
                CHECK(quantum == n);
                delivered += n;
            }

            CHECK_FALSE((sink >> nullptr));
        });

        CHECK(bool(residue >> undelivered));
        // csp::run waits for all imps to exit.
    });

    // All imps done — sent, delivered, undelivered are fully written.
    INFO(sent << " ≠ " << undelivered << " + " << delivered);
    CHECK(sent == undelivered + delivered);
}
