#include "testutil.h"

using namespace csp;

Logger g_log("test/bugs");

TEST_CASE("Bug - 2015_06_20") {
    reader<int> out;
    spawn([w = ++out]{
        csp_descr("outer");
        spawn([]{
            csp_descr("inner");
        });
        CSP_LOG(g_log, "");
        w << 2;
    });
    CSP_LOG(g_log, "");
    out.read();
}
