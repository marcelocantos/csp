# TODO

All work items are tracked as convergence targets in
[targets.md](targets.md).

## Open items

- ~~**Chat server shutdown crash**~~: Fixed. Both `Runtime::~Runtime()`
  and `Reactor::~Reactor()` now call `shutdown()`, joining their threads
  before static destruction destroys member variables.

- ~~**mbedTLS TSan false positives**~~: Resolved by replacing mbedTLS with
  PicoTLS (minicrypto backend). PicoTLS has zero internal mutexes, eliminating
  the TSan fiber-mutex interaction entirely.

- **`push_to_global` assertion with fake_clock + alt + multiple timers**:
  "Timer - MultipleTimersOrdering" crashes ~50% of the time under `csp::run`
  with `Assertion failed: (!imp->next_), function push_to_global, file
  runtime.cpp, line 133`. The test creates two `after` timers and uses `alt`
  to select the faster one. The quiescence-driven `advance_to_next` appears
  to trigger a race in imp scheduling. Test is currently skipped with
  `doctest::skip()` in `test/timer.test.cc`.
