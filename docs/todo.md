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

- ~~**`push_to_global` assertion with fake_clock + alt + multiple timers**~~:
  Resolved. The race (quiescence hook firing while imp is in a worker's local
  queue) is guarded by `if (next_) return;` in `Imp::schedule()`. Supporting
  fixes: cancel tokens for fake_clock timer entries, quiescence gap closure
  via `make_runnable` + atomic `qs_sleeping_`, and `alt_end_impl` reordering.
  Test runs and passes (not skipped).
