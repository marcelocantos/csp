#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    fprintf(stderr,
            "CSP_DIAG: CRASH exception=0x%08lX addr=%p rsp=%p\n",
            code, addr, (void*)ep->ContextRecord->Rsp);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv) {
    // Unbuffer output so crash diagnostics aren't lost in pipes.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
#endif

    fprintf(stderr, "CSP_DIAG: main() entered\n");
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    int res = context.run();
    fprintf(stderr, "CSP_DIAG: doctest finished, result=%d\n", res);
    return res;
}
