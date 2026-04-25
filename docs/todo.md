# TODO

All work items are tracked as convergence targets in
[bullseye.yaml](../bullseye.yaml).

## Open items

No open bugs. All previously tracked items resolved:

- ~~**Chat server shutdown crash**~~: Fixed. Destructor-based shutdown.
- ~~**mbedTLS TSan false positives**~~: Resolved by replacing mbedTLS with
  PicoTLS. Zero internal mutexes.
- ~~**`push_to_global` assertion with fake_clock + alt + multiple timers**~~:
  Resolved. Schedule guard + quiescence gap closure + alt_end reordering.
- ~~**Supervisor "restart under contention" SIGSEGV**~~: Resolved. HAMT
  use-after-free fix + exit_guard heap allocation + M:N supervisor transforms.
  Not reproduced in 25 stress runs with MALLOC_PERTURB_=42.
- ~~**Flaky timing tests under Linux MALLOC_PERTURB_**~~: Resolved on real
  hardware (CI green). Residual failures under QEMU x86_64 emulation are
  timing artifacts, not logic bugs.
- ~~**TLS concurrent connections SIGABRT on Linux**~~: Resolved by PicoTLS
  swap (no pthread mutexes under fiber migration).
