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
    FILE* f = fopen("csp_crash.log", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fflush(f);
        fclose(f);
    }
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
}

// VEH runs before SEH and doctest's handler — captures the real exception.
static LONG WINAPI veh_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Skip benign exceptions (breakpoints, output debug string, etc.)
    if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP ||
        code == DBG_PRINTEXCEPTION_C || code == 0x406D1388 /* SetThreadName */)
        return EXCEPTION_CONTINUE_SEARCH;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    char buf[512];
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        void* fault_addr = (void*)ep->ExceptionRecord->ExceptionInformation[1];
        int rw = (int)ep->ExceptionRecord->ExceptionInformation[0]; // 0=read, 1=write
        snprintf(buf, sizeof(buf),
                 "CSP_DIAG: VEH exception=0x%08lX (ACCESS_VIOLATION %s %p) "
                 "addr=%p rsp=%p rbp=%p",
                 code, rw ? "write" : "read", fault_addr,
                 addr, (void*)ep->ContextRecord->Rsp,
                 (void*)ep->ContextRecord->Rbp);
    } else {
        snprintf(buf, sizeof(buf),
                 "CSP_DIAG: VEH exception=0x%08lX addr=%p rsp=%p",
                 code, addr, (void*)ep->ContextRecord->Rsp);
    }
    write_crash_file(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Last-resort handler: fires when VEH and SEH both fail to handle
// the exception (e.g., double-fault during exception dispatch).
static LONG WINAPI uef_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    char buf[512];
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        void* fault_addr = (void*)ep->ExceptionRecord->ExceptionInformation[1];
        int rw = (int)ep->ExceptionRecord->ExceptionInformation[0];
        snprintf(buf, sizeof(buf),
                 "CSP_DIAG: UEF exception=0x%08lX (ACCESS_VIOLATION %s %p) "
                 "addr=%p rsp=%p rbp=%p",
                 code, rw ? "write" : "read", fault_addr,
                 addr, (void*)ep->ContextRecord->Rsp,
                 (void*)ep->ContextRecord->Rbp);
    } else {
        snprintf(buf, sizeof(buf),
                 "CSP_DIAG: UEF exception=0x%08lX addr=%p rsp=%p",
                 code, addr, (void*)ep->ContextRecord->Rsp);
    }
    write_crash_file(buf);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#ifdef _WIN32
    AddVectoredExceptionHandler(1, veh_handler);
    SetUnhandledExceptionFilter(uef_handler);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    std::set_terminate([] {
        write_crash_file("CSP_DIAG: std::terminate() called");
        auto ep = std::current_exception();
        if (ep) {
            try { std::rethrow_exception(ep); }
            catch (std::exception const& e) {
                char buf[512];
                snprintf(buf, sizeof(buf), "CSP_DIAG: terminate reason: %s", e.what());
                write_crash_file(buf);
            }
            catch (...) { write_crash_file("CSP_DIAG: terminate reason: unknown exception"); }
        }
        std::abort();
    });
#endif

    fprintf(stderr, "CSP_DIAG: main() entered\n");
    doctest::Context context;
    context.applyCommandLine(argc, argv);
    int res = context.run();
    fprintf(stderr, "CSP_DIAG: doctest finished, result=%d\n", res);
    return res;
}
