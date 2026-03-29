#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    // Default tests to CSP_MAXPROCS=1 unless overridden.  The runtime
    // always runs M:N (min 2 procs), so CSP_MAXPROCS=1 means 1 worker
    // thread + the main thread.  Tests that need more parallelism call
    // shutdown_runtime()/set_maxprocs() explicitly.
#ifdef _WIN32
    if (!std::getenv("CSP_MAXPROCS")) {
        _putenv_s("CSP_MAXPROCS", "1");
    }
#else
    if (!std::getenv("CSP_MAXPROCS")) {
        setenv("CSP_MAXPROCS", "1", 0);
    }
#endif

#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
#endif

    doctest::Context context;
    context.applyCommandLine(argc, argv);
    return context.run();
}
