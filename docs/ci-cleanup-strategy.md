# CI Cleanup Strategy — Comprehensive Issue Inventory

## Goal

Get ALL 9 CI jobs green on every run. No test toggles. Add branch
protection requiring all jobs pass.

## CI Matrix (`.github/workflows/ci.yml`)

| Job | Platforms | What it tests |
|---|---|---|
| `test` | macos-latest, ubuntu-latest, ubuntu-24.04-arm | Source build+test, dist gen, staleness check, dist test. Linux: `MALLOC_CHECK_=3 MALLOC_PERTURB_=42` |
| `sanitize` (x2) | macos-latest, ubuntu-latest, ubuntu-24.04-arm | `make test-dist SANITIZE=address,undefined` and `SANITIZE=thread`. `ASAN_OPTIONS=alloc_dealloc_mismatch=0` |

Total: 9 jobs (3 test + 6 sanitize). Currently NO branch protection —
jobs are informational only.

## Active Failures (as of 2026-03-03, run 22609674116)

4/9 jobs pass: all 3 `test` jobs + `thread (ubuntu-latest)`. 5 fail —
mapping to 4 issues below.

### Issue 1: Channel use-after-free in M:N mode (HIGHEST PRIORITY)

**Symptoms:**

- TSan `heap-use-after-free` in `pthread_mutex_lock` → `prialt_begin`
  (swap.test.cc:869, ubuntu-24.04-arm)
- ASan SIGSEGV in `Channel::prialt_begin_impl` at csp.cpp:572
  (mn.test.cc:361 "MN Volume - ManyChannelMessages", ubuntu-24.04-arm)

**Analysis:** A `Channel` object is destroyed while another imp on a
different OS thread still holds a live `chan_op` referencing its mutex.
The `chan_op` destructor calls `prialt_begin` on a freed channel. This is
a real concurrency bug in the channel lifecycle / endpoint refcounting
under M:N scheduling.

**Appears:** Flaky — shows up on ~50% of runs, mostly on
ubuntu-24.04-arm under both TSan and ASan.

**Fix approach:** Audit channel destruction vs `chan_op` lifetime. A
`chan_op` holds a raw pointer to a `Channel`. If the channel's last
endpoint is dropped on thread A while thread B's `chan_op` destructor
hasn't fired yet, the channel is freed too early. May need to prevent
channel destruction while any `chan_op` referencing it is alive (weak ref
or deferred destruction).

### Issue 2: LSan memory leaks in `context.test.cc` (CONSISTENT)

**Symptom:** ASan/LSan reports 320 bytes leaked in 6 allocations from
`jmpf()` at context.test.cc:35,108.

**Analysis:** The `jmpf()` test helper allocates a `std::function` on
the heap and passes it into a raw fcontext jump. The non-local jump
bypasses the destructor, leaking the heap block.

**Appears:** Consistently on ubuntu-latest ASan. LSan is enabled by
default with ASan.

**Fix approach:** Switch from heap-ownership to caller-managed storage.
Change `jmpf()` to take a reference (not rvalue+new), trampoline to use
non-owning pointer (not unique_ptr). Callers keep
`std::function<void()>` on main stack; destructors run when test scope
exits.

**Status:** Fixed in Phase 1.

### Issue 3: `chanutil.test.cc:895` Throttle budget reset flake (ASan macOS)

**Symptom:** `CHECK_FALSE( bool(r >> _) )` fails — throttle output has
extra value.

**Analysis:** Same root cause as the `dead_letter.test.cc` throttle
budget reset issue we fixed with fake_clock: when a tick and a sleep
expire at the same real time, the tick can reset the budget mid-burst.

**Fix approach:** Convert this test to fake_clock (same pattern as
dead_letter.test.cc). Use a sleep duration that's not a multiple of the
tick interval.

**Status:** Fixed in Phase 1.

### Issue 4: `mn.test.cc:691` Watchdog 200ms timing flake (TSan macOS)

**Symptom:** `CHECK( elapsed < 200ms )` fails under TSan. The watchdog
timer test exceeds its deadline.

**Analysis:** TSan adds 5-15x overhead. 200ms is too tight for TSan on
CI runners.

**Fix approach:** Cannot use fake_clock — test exercises real M:N
processor watchdog with OS threads and busy-loop stall. Widen threshold
with `CSP_TEST_SANITIZER` macro (from `testscale.h`):
`CHECK(elapsed < (CSP_TEST_SANITIZER ? 1000ms : 200ms))`.

**Status:** Fixed in Phase 1.

## Previously Fixed Issues (for reference)

- Linux ARM64 TPIDR_EL0 g_imp TLS caching → accessor functions in
  csp_globals.cpp
- Amalgamation TLS caching → separate csp_globals.cpp TU
- Scheduler exit race → three-way exit check with has_global_work_
- HAMT use-after-free in start() → retain before warmup switch
- Catch-block thread migration → capture exception_ptr, send outside
  catch
- alt dead-channel vs ready peer → defer dead reporting
- Work stealing stale running pointer → set under run_mu in do_switch
- Timer/pace/throttle/debounce timing flakes → fake_clock conversion

## Action Plan (ordered by impact)

### Phase 1: Fix the flakes (quick wins) — DONE

1. Convert `chanutil.test.cc:895` throttle test to fake_clock
2. Widen `mn.test.cc:691` watchdog threshold with CSP_TEST_SANITIZER
3. Fix `context.test.cc` LSan leak

### Phase 2: Fix the real bug

4. Diagnose and fix channel use-after-free in M:N mode (chan_op
   referencing freed Channel)
   - Get full TSan/ASan stack traces
   - Write TLA+ spec for channel destruction protocol
   - Fix: likely need reference counting or deferred destruction for
     Channel objects while chan_ops exist

### Phase 3: Enforce CI

5. Add branch protection rule requiring all 9 jobs to pass
6. Add `required_status_checks` to the workflow or configure via GitHub
   repo settings

## Branch Protection Config (for Phase 3)

In `.github/workflows/ci.yml`, no changes needed — the jobs already
exist. Just need GitHub repo settings:

- Settings → Branches → Add branch protection rule for `master`
- Require status checks: `test (macos-latest)`, `test (ubuntu-latest)`,
  `test (ubuntu-24.04-arm)`, `address,undefined (macos-latest)`,
  `address,undefined (ubuntu-latest)`,
  `address,undefined (ubuntu-24.04-arm)`, `thread (macos-latest)`,
  `thread (ubuntu-latest)`, `thread (ubuntu-24.04-arm)`
- Require branches to be up to date before merging

Alternatively, add a summary job that `needs: [test, sanitize]` and
require only that one job — simpler to maintain.
