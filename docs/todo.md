# TODO

All work items are tracked as convergence targets in
[targets.md](targets.md).

## Open items

- ~~**Chat server shutdown crash**~~: Fixed. Both `Runtime::~Runtime()`
  and `Reactor::~Reactor()` now call `shutdown()`, joining their threads
  before static destruction destroys member variables.

- **mbedTLS TSan false positives**: TSan reports hundreds of races in
  mbedTLS (`psa_reserve_free_key_slot`, `mbedtls_ctr_drbg_random_with_add`,
  `mbedtls_aesce_crypt_ecb`, `mbedtls_mpi_mul_mod`, etc.) during the
  "TLS - Concurrent connections" test. CSP already has `__tsan_switch_to_fiber`
  annotations in `switch_to()`, and mbedTLS key management IS supposed to be
  thread-safe with `MBEDTLS_THREADING_PTHREAD`. Both racing threads show
  `tid=0` in TSan output, suggesting TSan's fiber tracking is confused.
  Investigate whether the fiber annotations interact poorly with mbedTLS's
  pthread mutexes, or if there's a gap in annotation coverage (e.g., initial
  imp entry, worker loop transitions). This causes CI TSan jobs to fail with
  exit code 66 (dist) or SIGABRT (macOS TSan).

- **`push_to_global` assertion with fake_clock + alt + multiple timers**:
  "Timer - MultipleTimersOrdering" crashes ~50% of the time under `csp::run`
  with `Assertion failed: (!imp->next_), function push_to_global, file
  runtime.cpp, line 133`. The test creates two `after` timers and uses `alt`
  to select the faster one. The quiescence-driven `advance_to_next` appears
  to trigger a race in imp scheduling. Test is currently skipped with
  `doctest::skip()` in `test/timer.test.cc`.
