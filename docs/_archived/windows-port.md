# Windows Port

Design document for porting CSP to Windows, with the guiding principle
that the Windows implementation should fit naturally within Windows APIs
and idioms rather than emulating Unix.

## What doesn't change

The vast majority of CSP is pure C++ with no platform dependencies:

- Channels, alt/prialt, chan_op, two-phase protocol
- Imp lifecycle, spawn, join, yield
- M:N scheduler (std::thread, std::mutex, std::condition_variable)
- Dynamic scoping, HAMT
- All stream combinators (52 headers in `include/csp/part/`)
- Cancellation (cancel_guard, done, timed_out)
- Blocking pool
- Timer/clock public API (sleep, after, tick, fake_clock)
- fcontext.h declarations (extern "C" wrappers)

This is roughly 80% of the codebase.

## Platform-specific subsystems

### Reactor

The reactor's job is to wait for external OS events and deliver them as
channel death signals. The Unix implementation runs a dedicated thread
blocking in [`kevent`][kevent] (macOS) or [`epoll_wait`][epoll_wait]
(Linux).

On Windows, [`RegisterWaitForSingleObject`][regwait] is the native
primitive: hand the OS a [waitable handle][waitable] and a callback; the
system [thread pool][threadpool] invokes the callback when the handle is
signaled. The callback drops the `writer<>` endpoint — same death-signal
pattern, but **no dedicated reactor thread and no event loop**.

```
create_timer(delay_ns):
    h = CreateWaitableTimer(...)
    SetWaitableTimer(h, delay, ...)
    RegisterWaitForSingleObject(&wait, h, callback, ...)

create_fd_event(SOCKET s, fd_event ev):
    h = WSACreateEvent()
    WSAEventSelect(s, h, ev == read ? FD_READ|FD_CLOSE : FD_WRITE)
    RegisterWaitForSingleObject(&wait, h, callback, ...)

callback(ctx, timedOut):
    erase writer from map   // death signal fires
    notify park_cv           // wake scheduler
```

Timer events use [`CreateWaitableTimer`][createtimer] +
[`SetWaitableTimer`][settimer] to produce a waitable handle that signals
after the requested delay. Socket readiness uses
[`WSAEventSelect`][wsaeventselect] to bind socket events to a
[`WSACreateEvent`][wsacreateevent] handle. Both are then registered with
[`RegisterWaitForSingleObject`][regwait].

The scheduler integration (`has_pending_signals`, `park_cv` wakeup) is
identical — the callback does the same `pending_signals_` decrement and
`park_cv.notify` that the Unix reactor loop does.

Reactor header members become platform-specific:

```cpp
#ifdef __APPLE__
    int kq_ = -1;
#elif defined(__linux__)
    int epfd_ = -1;
    int wakefd_ = -1;
    std::unordered_map<int, uintptr_t> timerfd_to_ident_;
    std::unordered_map<uintptr_t, int> ident_to_timerfd_;
#elif defined(_WIN32)
    // Each registered wait: the waitable handle + registration handle.
    struct WaitEntry { HANDLE object; HANDLE wait; };
    std::unordered_map<uintptr_t, WaitEntry> timer_waits_;
    std::unordered_map<SOCKET, WaitEntry>    fd_waits_;
#endif
```

Shutdown calls [`UnregisterWaitEx`][unregwait] for each entry instead of
closing a kqueue/epoll fd.

[kevent]: https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/kevent.2.html
[epoll_wait]: https://man7.org/linux/man-pages/man2/epoll_wait.2.html
[regwait]: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-registerwaitforsingleobject
[waitable]: https://learn.microsoft.com/en-us/windows/win32/sync/wait-functions#waitable-objects
[threadpool]: https://learn.microsoft.com/en-us/windows/win32/procthread/thread-pooling
[createtimer]: https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerw
[settimer]: https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimer
[wsaeventselect]: https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaeventselect
[wsacreateevent]: https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsacreateevent
[unregwait]: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-unregisterwaitex

### Stack pool

Direct translation — same structure, different function names:

| Unix | Windows |
|---|---|
| [`mmap`][mmap]`(MAP_ANON\|MAP_PRIVATE)` | [`VirtualAlloc`][virtualalloc]`(MEM_RESERVE\|MEM_COMMIT, PAGE_READWRITE)` |
| [`mprotect`][mprotect]`(base, page_size, PROT_NONE)` | [`VirtualProtect`][virtualprotect]`(base, page_size, PAGE_NOACCESS, &old)` |
| [`munmap`][munmap]`(base, size)` | [`VirtualFree`][virtualfree]`(base, 0, MEM_RELEASE)` |
| [`madvise`][madvise]`(MADV_FREE)` | [`DiscardVirtualMemory`][discard] or [`VirtualFree`][virtualfree]`(addr, len, MEM_DECOMMIT)` |
| [`getpagesize()`][getpagesize] | [`GetSystemInfo`][getsysteminfo]`(&si); si.dwPageSize` |

[mmap]: https://man7.org/linux/man-pages/man2/mmap.2.html
[mprotect]: https://man7.org/linux/man-pages/man2/mprotect.2.html
[munmap]: https://man7.org/linux/man-pages/man2/munmap.2.html
[madvise]: https://man7.org/linux/man-pages/man2/madvise.2.html
[getpagesize]: https://man7.org/linux/man-pages/man2/getpagesize.2.html
[virtualalloc]: https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc
[virtualprotect]: https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotect
[virtualfree]: https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualfree
[discard]: https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-discardvirtualmemory
[getsysteminfo]: https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getsysteminfo

### I/O

`csp::io` is socket I/O on both platforms. The API shape is the same,
the types differ:

| | Unix | Windows |
|---|---|---|
| Handle type | `int` | [`SOCKET`][socket_type] |
| Read | [`read`][read]`(fd, buf, len)` | [`recv`][recv]`(s, (char*)buf, len, 0)` |
| Write | [`write`][write]`(fd, buf, len)` | [`send`][send]`(s, (char*)buf, len, 0)` |
| Accept | [`accept`][accept_posix]`(fd, ...)` | [`accept`][accept_win]`(s, ...)` |
| Connect | [`connect`][connect_posix]`(fd, ...)` | [`connect`][connect_win]`(s, ...)` |
| Non-block | [`fcntl`][fcntl]`(F_SETFL, O_NONBLOCK)` | [`ioctlsocket`][ioctlsocket]`(s, FIONBIO, &one)` |
| Close | [`close`][close]`(fd)` | [`closesocket`][closesocket]`(s)` |
| Error check | `errno == EAGAIN` | [`WSAGetLastError`][wsagetlasterror]`() == WSAEWOULDBLOCK` |
| In-progress | `errno == EINPROGRESS` | `WSAGetLastError() == WSAEWOULDBLOCK` |
| Interrupted | `errno == EINTR` (handle) | doesn't happen in Winsock |
| Invalid | `-1` | [`INVALID_SOCKET`][invalid_socket] |
| Size type | `ssize_t` | `int` |

All substitutions are in `io.cc` — user code calls `csp::io::read(s, buf, len)`
identically.

`resolve` (DNS) uses [`getaddrinfo`][getaddrinfo_posix] on both
platforms — Winsock provides the [same function][getaddrinfo_win].
Already runs on the blocking pool, which is portable.

Winsock requires [`WSAStartup`][wsastartup] / [`WSACleanup`][wsacleanup]
lifecycle management. Natural home: runtime auto-initialization / shutdown.

[socket_type]: https://learn.microsoft.com/en-us/windows/win32/winsock/socket-data-type-2
[read]: https://man7.org/linux/man-pages/man2/read.2.html
[write]: https://man7.org/linux/man-pages/man2/write.2.html
[recv]: https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-recv
[send]: https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-send
[accept_posix]: https://man7.org/linux/man-pages/man2/accept.2.html
[accept_win]: https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-accept
[connect_posix]: https://man7.org/linux/man-pages/man2/connect.2.html
[connect_win]: https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-connect
[fcntl]: https://man7.org/linux/man-pages/man2/fcntl.2.html
[ioctlsocket]: https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-ioctlsocket
[close]: https://man7.org/linux/man-pages/man2/close.2.html
[closesocket]: https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-closesocket
[wsagetlasterror]: https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-wsagetlasterror
[invalid_socket]: https://learn.microsoft.com/en-us/windows/win32/winsock/socket-data-type-2
[getaddrinfo_posix]: https://man7.org/linux/man-pages/man3/getaddrinfo.3.html
[getaddrinfo_win]: https://learn.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-getaddrinfo
[wsastartup]: https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-wsastartup
[wsacleanup]: https://learn.microsoft.com/en-us/windows/win32/api/winsock/nf-winsock-wsacleanup

### Signals vs waitable handles

`csp::signal` is POSIX-specific ([`sigaction`][sigaction], [self-pipe
trick][selfpipe]). It doesn't exist on Windows. Instead, Windows gets
`csp::win` — a different feature that fits the Windows event model:

```cpp
// include/csp/win.h
namespace csp::win {
    // Bridge any waitable HANDLE into a CSP death signal.
    reader<> notify(HANDLE h);

    // Console control events (Ctrl-C, Ctrl-Break, close).
    reader<DWORD> console_ctrl();
}
```

`notify(HANDLE)` reuses the reactor's [`RegisterWaitForSingleObject`][regwait]
machinery to bridge **any** [waitable handle][waitable] into a channel —
[event objects][event_objects], [waitable timers][waitable_timers],
[process/thread handles][process_handles],
[named pipe][named_pipes] events,
[file system change notifications][findchangenotification]. This is
strictly more general than `csp::signal::notify`.

`console_ctrl()` uses [`SetConsoleCtrlHandler`][consolectrl] internally
and delivers [`CTRL_C_EVENT`][ctrl_events], `CTRL_BREAK_EVENT`, etc. as
channel values.

`csp::signal` keeps its name (no `unix::` prefix). It's already behind
a platform-specific header. On Windows, the header doesn't exist. The
`csp::win` namespace earns its prefix because it wraps Windows-specific
types (`HANDLE`, `DWORD`) that don't exist elsewhere.

[sigaction]: https://man7.org/linux/man-pages/man2/sigaction.2.html
[selfpipe]: https://cr.yp.to/docs/selfpipe.html
[event_objects]: https://learn.microsoft.com/en-us/windows/win32/sync/event-objects
[waitable_timers]: https://learn.microsoft.com/en-us/windows/win32/sync/waitable-timer-objects
[process_handles]: https://learn.microsoft.com/en-us/windows/win32/procthread/process-handles-and-identifiers
[named_pipes]: https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipes
[findchangenotification]: https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstchangenotificationw
[consolectrl]: https://learn.microsoft.com/en-us/windows/console/setconsolectrlhandler
[ctrl_events]: https://learn.microsoft.com/en-us/windows/console/handlerroutine

### TLS (PicoTLS)

Socket I/O in `conn`: [`read`][read]/[`write`][write] →
[`recv`][recv]/[`send`][send] (operating on sockets). PicoTLS itself
is buffer-based with no platform dependencies. The minicrypto backend
has no threading primitives, so no platform-specific mutex changes
are needed (unlike the former mbedTLS integration).

`F_SETNOSIGPIPE` is already `#ifdef`-guarded. Windows has no
[`SIGPIPE`][sigpipe].

[sigpipe]: https://man7.org/linux/man-pages/man7/signal.7.html

### Small items

| Item | Change |
|---|---|
| Thread naming (`csp.cc`) | [`SetThreadDescription`][setthreaddesc]`(GetCurrentThread(), ...)` (Win 10 1607+) |
| Backtrace (`log.cc`) | [`CaptureStackBackTrace`][capturestackbt] + [`SymFromAddr`][symfromaddr], or `#ifdef` stub |
| `ssize_t` | `typedef ptrdiff_t ssize_t` under `_WIN32` |
| fcontext assembly | Windows PE variants exist in [Boost.Context][boost_context] vendor tree; build system selects them |
| `<unistd.h>` | `#ifndef _WIN32` guard; `<io.h>` where needed on Windows |

[setthreaddesc]: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreaddescription
[capturestackbt]: https://learn.microsoft.com/en-us/windows/win32/debug/capturestackbacktrace
[symfromaddr]: https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-symfromaddr
[boost_context]: https://www.boost.org/doc/libs/release/libs/context/doc/html/context/overview.html

### Stack analysis

The ARM64 instruction walker (`stack_analysis_arm64.cc`) has a
conservative fallback for non-aarch64 — returns `{32KB, false}`. No
porting work needed for x86_64 Windows. An ARM64 Windows port would
already be covered by the existing analyzer.

## File organisation

### Principle

**Same feature, different implementation** → `#ifdef` branches in the
same file (reactor, stack_pool, io, thread naming, backtrace).

**Different feature** → different file (`signal.cc` is POSIX-only,
`win.cc` is Windows-only).

### New files

| File | Contents |
|---|---|
| `include/csp/win.h` | `csp::win::notify(HANDLE)`, `csp::win::console_ctrl()` |
| `src/win.cc` | Implementation of the above |

### Modified files (platform branches added)

| File | Change |
|---|---|
| `src/reactor.cc` | `#elif defined(_WIN32)` branch (~180 lines) |
| `include/csp/internal/reactor.h` | Windows member block |
| `src/stack_pool.cc` | `#elif defined(_WIN32)` branch (~40 lines) |
| `src/io.cc` | Winsock variants of syscall wrappers |
| `include/csp/io.h` | `SOCKET` parameter type on Windows |
| `include/csp/internal/signal.h` | `fd_signal::fd_` becomes `SOCKET` on Windows |
| `include/csp/signal.h` | `#ifndef _WIN32` guard |
| `src/signal.cc` | `#ifndef _WIN32` guard |
| `src/csp.cc` | [`SetThreadDescription`][setthreaddesc] branch |
| `src/log.cc` | [`CaptureStackBackTrace`][capturestackbt] or stub |
| `src/tls.cc` | [`recv`][recv]/[`send`][send] in BIO callbacks |
| TLS (PicoTLS) | No platform changes needed (buffer-based, no mutexes) |

### Distribution (amalgamation)

The three-file distribution stays: `csp.h`, `csp.cpp`, `csp_globals.cpp`.

Platform branches within the amalgamated files handle everything —
`#ifdef` selects the right code at compile time. A Windows dist user
compiles the same three files with MSVC or [Clang-cl][clangcl].

`win.cc` is amalgamated into `csp.cpp` with `#ifdef _WIN32` guards.
`signal.cc` content is guarded with `#ifndef _WIN32`. The amalgamation
script (`scripts/amalgamate.py`) doesn't need structural changes — it
already concatenates all source files and lets preprocessor guards sort
out platform selection.

`win.h` is amalgamated into `csp.h` similarly. The gateway header
(`include/csp.h`) adds `#include "csp/win.h"` — the header's own
`#ifdef _WIN32` guard prevents it from affecting Unix builds.

[clangcl]: https://clang.llvm.org/docs/MSVCCompatibility.html

### fcontext assembly in distribution

The amalgamation already converts `.S` files to inline `asm()` blocks
with architecture/platform guards. Windows PE variants
(`jump_x86_64_ms_pe_clang_gas.S`, etc.) need to be added to
`amalgamate_fcontext()`. MSVC doesn't support GAS inline assembly, so
the PE `.S` files would need to be compiled separately ([MASM][masm]
`.asm` files or linked as object files). This is the one area where the
distribution workflow diverges: MSVC users may need a pre-compiled
fcontext object file alongside the three source files.

[Clang-cl][clangcl] supports GAS syntax, so Clang-cl users get the same
inline `asm()` approach that works today.

[masm]: https://learn.microsoft.com/en-us/cpp/assembler/masm/masm-for-x64-ml64-exe

## Build system

Add a `CMakeLists.txt` for cross-platform builds. The Makefile stays
for Unix developers. [CMake][cmake] is what Windows developers expect.

The CMakeLists.txt selects platform-specific source files and assembly
variants. It handles Winsock linking (`ws2_32.lib`), and conditionally
includes `win.cc` or `signal.cc` based on platform.

[cmake]: https://cmake.org/cmake/help/latest/

## Compiler considerations

**[Clang-cl][clangcl]** (Clang with MSVC ABI) is the easier first
target — the codebase is already Clang-only, GCC/Clang extensions work,
and GAS inline assembly works.

**MSVC** requires:
- [`__declspec(noinline)`][declspec_noinline] instead of
  `__attribute__((noinline))`
- No `__builtin_frame_address` — use [`_ReturnAddress`][returnaddress]
  or stub
- No GAS inline assembly — fcontext needs [MASM][masm] `.asm` or
  pre-compiled object files
- `_WIN32` is [defined automatically][predefined_macros]

[declspec_noinline]: https://learn.microsoft.com/en-us/cpp/cpp/noinline
[returnaddress]: https://learn.microsoft.com/en-us/cpp/intrinsics/returnaddress
[predefined_macros]: https://learn.microsoft.com/en-us/cpp/preprocessor/predefined-macros

## Phasing

1. **Stack pool + fcontext** — get imps running. Test spawn/yield.
2. **Reactor ([`RegisterWaitForSingleObject`][regwait])** — timers and
   socket events. Test after(), tick(), sleep().
3. **I/O wrappers** — [Winsock][winsock] layer. Test loopback TCP.
4. **csp::win** — console_ctrl() and notify(HANDLE).
5. **Build** — CMakeLists.txt.
6. **TLS** — PicoTLS: socket I/O in conn (recv/send instead of read/write).
7. **Distribution** — update amalgamate.py for Windows fcontext variants.

[winsock]: https://learn.microsoft.com/en-us/windows/win32/winsock/getting-started-with-winsock
