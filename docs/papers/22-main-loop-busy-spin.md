# 22. Main-loop busy-spin when quiescent without hook

**Date**: 2026-05-13
**Status**: diagnosed, not yet fixed
**Related targets**: 🎯T26 (web_crawler example reliability), 🎯T27 (this runtime bug)

## Symptom

When a CSP program reaches a quiescent-but-not-done state (all worker procs parked, no runnable imps in the global queue, `user_done()` false, no quiescence hook registered), the main thread enters a tight busy-spin at 99% CPU in `Runtime::main_loop()` at `src/runtime.cpp:296-324`.

Reproduced by `examples/web_crawler` ≈50% of the time on macOS arm64. The example's HTTP-server-plus-crawler-workers topology occasionally reaches a state where one or more imps are suspended waiting on a TCP read that never completes; the runtime then spins.

## Actors

The wait loop in `Runtime::main_loop` (paraphrased):

```cpp
for (;;) {                                             // line 296
    {
        std::unique_lock<std::mutex> lk(park_mu);      // 298
        park_cv.wait(lk, [&] {                          // 299
            if (user_done()) return true;
            if (has_global_work_) return true;
            return all_parked();   // ← quiescent: hook should fire
        });
    }                                                   // 304 — drop lk
    if (user_done()) break;                             // 305
    if (has_global_work_) continue;                     // 306
    {
        std::lock_guard<std::mutex> hlk(hook_mu_);     // 309
        if (quiescence_hook_) { … fire and continue; }
    }
    continue;                                           // 323
}
```

`all_parked()` returns true iff every alive worker proc (i ≥ 1) has its `parked` flag set. This is the condition the *fake-clock* tests use to advance simulated time via the hook.

## What's wrong

When `all_parked()` is true and `quiescence_hook_` is null:

1. `wait` returns immediately (predicate true).
2. `user_done`, `has_global_work_` both false.
3. Hook is null → fall through.
4. `continue` → top of loop.
5. Re-enter `wait` → predicate evaluates again → `all_parked()` is *still* true → return true → loop.

Result: continuous lock/check/unlock/lock/check/… cycle that consumes 99% of one core until something external (a reactor event, a signal) flips `user_done` or `has_global_work_`.

In the web_crawler case, the external event never arrives (true deadlock in the example), so the spin runs until SIGKILL.

## Hypothesis

The predicate over-eagerly wakes for the quiescent state. The wake exists to let the hook fire; without a hook, treating quiescence as a wake condition is wrong — the runtime should genuinely sleep until either `user_done` or `has_global_work_` flips.

## Fix candidates

**A. Make the hook check part of the predicate.** Only wake on quiescence if a hook is registered:

```cpp
park_cv.wait(lk, [&] {
    if (user_done()) return true;
    if (has_global_work_.load(std::memory_order_acquire)) return true;
    if (has_hook_.load(std::memory_order_acquire) && all_parked()) return true;
    return false;
});
```

Requires `has_hook_` as an `atomic<bool>` mirror of `quiescence_hook_`'s registered state, updated atomically when the hook is installed/cleared. Avoids touching `hook_mu_` inside the predicate (which would interact badly with the existing locking discipline).

Trade-off: silent under genuine deadlock — the program parks forever instead of burning CPU. The user notices via a wall-clock hang either way; CPU usage just stops being a tell.

**B. Backoff under quiescence.** Keep current logic but sleep ~1 ms between iterations when the predicate is the quiescence path. Trivial to implement; preserves the existing "diagnostically loud" deadlock signature. Trade-off: tiny per-iteration cost in tests using fake_clock.

**C. Separate the test-mode hook path.** Introduce a second cv (`quiescent_cv`) that's notified only when a hook is registered or fake-clock work appears. main_loop waits on `park_cv` for production, on `quiescent_cv` for test mode. Cleaner separation but more code churn.

**Recommendation**: A, with the atomic mirror. Smallest diff, preserves test-mode behavior, eliminates production busy-spin. A unit test (asserts <5% CPU during a 1s `schedule()` on a deadlocked imp graph) would pin the fix.

## Companion bug (separate)

The runtime busy-spin is the *symptom*; the underlying cause in web_crawler is a real deadlock — some imp waiting on a TCP read whose other end never produces FIN. That's a separate target (🎯T26's example-level fix). Even after fixing the busy-spin, web_crawler will still hang on a wall clock — but the CI safety-net timeout will catch it and burn no CPU in the meantime.

## Reproduction

```bash
make build
./build/normal/examples/web_crawler        # ~50% chance to hang at 99% CPU
sample $(pgrep web_crawler) 1 100          # confirms spin in main_loop runtime.cpp:296-324
```

The same binary, invoked via `make run-examples-ci EXAMPLE_CI_SKIP="chat_server task_scheduler"`, also reproduces — the make wrapper is not causally relevant (earlier hypothesis was wrong; the flakiness is just sometimes-passes).
