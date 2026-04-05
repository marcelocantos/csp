#include "testutil.h"

using namespace csp;

Logger g_log("test/bugs");

TEST_CASE("Bug - 2015_06_20") {
    csp::run([]{
        reader<int> out;
        spawn([w = ++out]{
            csp::internal::descr("outer");
            spawn([]{
                csp::internal::descr("inner");
            });
            CSP_LOG(g_log, "");
            w << 2;
        });
        CSP_LOG(g_log, "");
        out.read();
    });
}
