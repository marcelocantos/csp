# TODO

All work items are tracked as convergence targets in
[targets.md](targets.md).

## Open items

- **Chat server shutdown crash**: `libc++abi: terminating` (SIGABRT) on
  SIGTERM shutdown. `std::terminate()` called with no current exception —
  likely a joinable `std::thread` destructor in the M:N runtime during
  cancellation teardown. Core functionality works; this is a cosmetic
  shutdown issue. See 🎯T7.1.
