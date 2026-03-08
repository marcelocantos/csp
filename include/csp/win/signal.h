#pragma once

#ifdef _WIN32

#include <csp/csp.h>

#include <windows.h>

#include <initializer_list>

namespace csp::win::signal {

// Returns a reader that emits the console control event type (DWORD) each
// time one of the specified events is delivered to the process. Installs a
// console control handler via SetConsoleCtrlHandler (once, idempotent).
// Requires init_runtime() since it uses the I/O reactor internally.
//
// Dropping the returned reader stops delivery and cleans up the underlying
// socket pair. The handler remains installed but becomes a harmless no-op
// once the event mask is cleared.
//
// Supported events: CTRL_C_EVENT, CTRL_BREAK_EVENT, CTRL_CLOSE_EVENT,
//                   CTRL_LOGOFF_EVENT, CTRL_SHUTDOWN_EVENT.
//
// Usage:
//   auto sig = csp::win::signal::notify({CTRL_C_EVENT, CTRL_CLOSE_EVENT});
//   prialt(
//       data >> [](auto x) { process(x); },
//       sig  >> [](DWORD e) { shutdown(e); }
//   );
reader<DWORD> notify(std::initializer_list<DWORD> events);

// Inject a console control event directly into the signal machinery,
// bypassing GenerateConsoleCtrlEvent (which sends to the entire process
// group and kills the CI runner). Safe for use in tests.
void raise(DWORD event);

}

#endif // _WIN32
