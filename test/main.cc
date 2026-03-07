#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static void write_crash_file(const char* msg) {
    // Write to a file since stderr through PowerShell pipes may be lost.
    FILE* f = fopen("csp_crash.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fflush(f);
        fclose(f);
    }
    // Also try stderr.
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "CSP_DIAG: CRASH exception=0x%08lX addr=%p rsp=%p",
             code, addr, (void*)ep->ContextRecord->Rsp);
    write_crash_file(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void csp_terminate_handler() {
    write_crash_file("CSP_DIAG: std::terminate() called");
    abort();
}

static void csp_atexit() {
    write_crash_file("CSP_DIAG: atexit handler called (normal exit path)");
}
#endif

int main(int argc, char** argv) {
    // Unbuffer output so crash diagnostics aren't lost in pipes.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
    std::set_terminate(csp_terminate_handler);
    atexit(csp_atexit);
    // Suppress Windows error dialog boxes.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
#endif

    fprintf(stderr, "CSP_DIAG: main() entered\n");
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    int res = context.run();
    fprintf(stderr, "CSP_DIAG: doctest finished, result=%d\n", res);
    return res;
}
