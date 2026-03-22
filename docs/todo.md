# TODO

All work items are tracked as convergence targets in
[targets.md](targets.md).

## Open items

- **Chat server shutdown crash**: `libc++abi: terminating` (SIGABRT) on
  SIGTERM shutdown. The general joinable-thread-destructor issue was fixed
  (`Runtime::~Runtime()` calls `shutdown()`), but the chat server still
  crashes — specific to signal/cancellation/I/O teardown path. Simpler
  examples (rpc_service, standalone programs) now exit cleanly. See 🎯T7.1.

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
