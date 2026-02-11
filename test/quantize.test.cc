#include "testutil.h"

#include <csp/enumerate.h>
#include <csp/quantize.h>

using namespace csp;

static Logger g_log("Quantize.Test");

TEST_CASE("Quantize - Simple") {
    RunStats stats;

    writer<int> in, quanta;
    reader<int> out, residue;

    stats.spawn(chan::quantize(--in, --quanta, ++out, ++residue));

    quanta << 5; quanta = {};
    in << 7; in = {};
    CHECK_EQ(5, out.read());
    CHECK_EQ(2, residue.read());
    csp::schedule();
}

TEST_CASE("Quantize - Complex") {
    RunStats stats;

    int loops = 11;

    int sent = 0, delivered = 0, undelivered = 0;

    channel<int> source;
    stats.spawn([loops, w = ++source, &sent]{
        constexpr int delta = 23;
        for (int i = 0; i < loops * (7 + 13 + 11) && w << delta; i += delta) {
            CSP_LOG(g_log, "  \033[31mi=%3d δ=%d\033[0m", i, delta);
            sent += delta;
        }
    });

    std::vector<int> qdata = {7, 13, 11};

    channel<int> quanta;
    reader<int> residue;
    stats.spawn(chan::enumerate(std::vector<int>{qdata}, ++quanta, true));

    reader<int> sink;
    stats.spawn(chan::quantize(--source, --quanta, ++sink, ++residue));

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
                CHECK_EQ(n, v);
            }
        }
        int dummy;
        INFO(dummy);
        CHECK_FALSE((sink >> dummy));
    });

    CHECK(bool(residue >> undelivered));
    csp::schedule();

    INFO(sent << " ≠ " << undelivered << " + " << delivered);
    CHECK_EQ(sent, undelivered + delivered);
}

TEST_CASE("Quantize - Uniform") {
    RunStats stats;

    int sent = 0, delivered = 0, undelivered = 0;

    channel<int> source;
    stats.spawn([source = ++source, &sent]{
        constexpr int delta = 23;
        for (int i = 0; i < 13 * 7 && source << delta; i += delta) {
            sent += delta;
        }
    });

    constexpr int quantum = 7;

    reader<int> sink;
    reader<int> residue;
    stats.spawn(chan::quantize(--source, quantum, ++sink, ++residue));

    stats.spawn([sink = std::move(sink), quantum, &delivered]{
        for (int n; sink >> n;) {
            CHECK_EQ(quantum, n);
            delivered += n;
        }

        CHECK_FALSE((sink >> nullptr));
    });

    CHECK(bool(residue >> undelivered));
    csp::schedule();

    INFO(sent << " ≠ " << undelivered << " + " << delivered);
    CHECK_EQ(sent, undelivered + delivered);
}
