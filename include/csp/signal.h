#pragma once

#ifndef _WIN32

#include <csp/csp.h>

#include <initializer_list>

namespace csp::signal {

// Returns a reader that emits the signal number each time one of the
// specified signals is delivered to the process. Installs signal
// handlers via sigaction (idempotent per signal number). Requires
// init_runtime() since it uses the I/O reactor internally.
//
// Dropping the returned reader stops delivery and cleans up the
// underlying pipe. Signal handlers remain installed (harmless no-op
// once no pipes care about the signal).
//
// Usage:
//   auto sig = csp::signal::notify({SIGINT, SIGTERM});
//   prialt(
//       data >> [](auto x) { process(x); },
//       sig  >> [](int s)  { shutdown(s); }
//   );
reader<int> notify(std::initializer_list<int> sigs);

}

#endif // !_WIN32
