#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>

int main(int argc, char** argv) {
    fprintf(stderr, "CSP_DIAG: main() entered\n");
    fflush(stderr);
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    int res = context.run();
    fprintf(stderr, "CSP_DIAG: doctest finished, result=%d\n", res);
    return res;
}
