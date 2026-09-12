# 36 — A sleeping worker with runnable work

Date: 2026-09-12.

## Observation

The first full macOS ARM64 ASan+UBSan distributed suite after the September
audit integration stopped in `diff / constant-input---all-zeros`. XML test
progress located the case; a live stack sample showed 0% CPU, the main
thread waiting on completion, and the only worker blocked in `Note::sleep`.
An LLDB core captured:

- `global_run_queue.size() == 1`, `has_global_work_ == true`;
- `live_gs == 2`, `daemon_gs == 1`;
- the worker's local ring contained only its sentinel, `running == nullptr`,
  `parked == true`, and `note.val_ == SLEEPING`;
- the queued imp's entry was the test body, which had not begun execution.

The pipeline had not deadlocked: its initial runnable imp had no awake
worker. Native source/distribution suites, with TLS enabled and disabled,
and a shuffled native source suite had passed before this capture. Those
passes do not invalidate the captured failure.

## Lost wake between two compare-and-exchanges

The old `Note::wake` performed two attempts without retrying:

1. The worker prepares to sleep while the Note is `AWAKE`.
2. The waker's `SLEEPING → AWAKE` CAS fails, observing `AWAKE`.
3. The worker's `AWAKE → SLEEPING` CAS succeeds.
4. The waker's `AWAKE → FLAGGED` CAS fails, observing `SLEEPING`.
5. The waker returns without changing the Note or issuing a wake syscall.

The fix retries when the second CAS observes `SLEEPING`. A successful
flag or an already pending flag still completes immediately. The protocol
and its broken twin are in [NoteWake.tla](../../formal/NoteWake.tla) and
[NoteWake_Bug.tla](../../formal/NoteWake_Bug.tla). The old
[PerWorkerWake model](../../formal/PerWorkerWake.tla) combined the wake
decision into one atomic action and could not expose this implementation
gap.

The new `Note / wake-racing-sleep-entry-is-not-lost` C++ regression races
the real Note between two OS threads. It has a bounded wait and only calls
a timeout a lost wake if `wake()` returned before the wait's deadline.
Compiled against the original header, it failed with `lost_wake == true`;
compiled against the fix, it passed. A separate 100,000-attempt run of the
fixed Note completed with zero timeouts. The fixed model checks 10 distinct
states; the broken twin produces the five-state trace above.

## Publication ordering at the parking boundary

Independent review found another gap after `has_work` stopped acquiring
`global_mu`. The worker publishes `parked`, executes the SC fence inside
`notify_quiesce_watchers`, then reads `has_global_work_`. The publisher
publishes work and scans the workers' Notes and `parked` flags. Its side
lacked a corresponding SC fence.

Release stores and acquire loads alone permit both sides to observe old
values: the publisher sees no parked worker, while the worker sees no work
and goes to sleep. A fence at the start of `wake_a_worker`, before either
scan, pairs with the worker's existing fence. If both reads missed, parked
coherence would require publisher-fence before worker-fence, while work
coherence would require the opposite order. The SC total order excludes
that cycle. The worker fence must remain before the zero-waiter fast return.

[WorkPublication.tla](../../formal/WorkPublication.tla) explores a bounded
store-buffer abstraction of this boundary. Its fixed variant checks 36
distinct states; its broken twin reaches `NoStrandedWork` failure. It is
an abstraction with an adjacent C++ memory-order argument, not a complete
C++ memory-model checker.

## Verification and limits

The integrated cancellation and worker-wake fixes passed the complete
configured suites on 2026-09-12:

| Platform / instrumentation | TLS | Source cases | Distribution cases |
|---|---|---:|---:|
| macOS ARM64, normal | enabled | 785 | 769 |
| macOS ARM64, normal | disabled | 765 | 749 |
| macOS ARM64, ASan + UBSan | enabled | 757 | 741 |
| macOS ARM64, ASan + UBSan | disabled | 737 | 721 |
| Linux ARM64, TSan | disabled | 737 | 721 |

All rows passed with zero test failures and configuration-matched inventory
parity. The sanitizer runs retained existing exclusions and documented
runtime options; UBSan and TSan used `halt_on_error=1`. Linux x86_64 and
Windows were not rerun in this local verification pass.

Both fixes preserve the existing blocking and wake behavior; they add no
polling timeout or periodic rescue to conceal a missed notification. The
Note regression runs in both source and distribution suites. Both positive
models join the existing `make check` inventory, and their deliberately
broken twins retain executable counterexamples.

The core identifies the stranded-work condition, but cannot distinguish
which of the two lost-wake mechanisms produced that particular state.
Earlier historical hangs lack equivalent captures, so this is not proof
that every occurrence of 🎯T29 shared these causes.

Distribution coverage now compares the complete compiled source and dist
test inventories for the same configuration. `make test-dist` checks both
TLS configurations, and also checks the TLS-disabled ThreadSanitizer
configuration. It preserves duplicate names, accounts for the explicit
white-box exemptions, and fails on listing errors, malformed or missing
inventories, mismatched configurations, and paths to the same binary.
The oracle no longer depends on a manually maintained protocol-file list.

For a future investigation, retain named test progress and the hung binary
alongside its core. For example, the existing make override supports:

```sh
make test-dist TEST_RUN='gtimeout -s QUIT --kill-after=60 1200 ./$(TARGET) --no-colors --reporters=xml'
```

Use `timeout` on Linux. Store diagnostic logs and cores in a scratch
directory outside the repository. A passing rerun is counterevidence,
not an explanation for the original stall.
