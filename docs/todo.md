# TODO

All work items are tracked as convergence targets in
[bullseye.yaml](../bullseye.yaml).

## Open items

### Stack analyzer: data-aware branch pruning for closure-loaded integers (🎯T3.4)

The CONST register tracking added in the 2026-04-27 session handles MOVZ/MOVK
constants but not integers loaded from the closure (DATA_OFFSET fields). To
prune branches like `if (d->tag == 0) { ... }` at the evaluator level, one of:

- **Option A** (recommended): add `OP_BRANCH_CONST <data_offset> <branch_target_delta>`
  opcode. The evaluator reads `*(current_data + offset)` and skips to
  `branch_target_delta` bytes into the bytecode if the condition holds. Walker
  emits this opcode at CBZ/CBNZ/TBZ/TBNZ when the tested register is
  DATA_OFFSET (not CONST). See paper 08 §11.3 for design.

- **Option B**: data-aware re-walk — call `walk()` a second time with the actual
  data pointer set in the initial register state, resolve DATA_OFFSET loads to
  CONST values, and use the result for data-specific depth queries. Heavier but
  avoids new opcodes.

### Stack analyzer: PC_RELATIVE BLR resolution for internal dispatch tables (🎯T3.4)

ADRP+LDR+BLR patterns that go through GOT/PLT stubs are intentionally not
followed (they could walk into external libraries and SIGSEGV). However,
ADRP-resolved BLRs that point to internal dispatch tables (e.g. a static
function pointer array in the same binary) could safely be followed if we can
distinguish them from stubs.

Guard approach: before dereferencing a PC_RELATIVE address in BLR/BR, check
that the resulting function pointer falls within the calling binary's `__TEXT`
section (excluding `__stubs`). Can use `dl_iterate_phdr` (Linux) or
`_dyld_get_image_header` (macOS) to get segment boundaries at init time.

### Profile-guided stack budget calibration (🎯T3.4)

See paper 08 §6. When an imp exits its budget, record the high-water mark and
use it to calibrate per-function budgets for future spawns. Low implementation
effort, moderate accuracy gain for long-running programs.

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
