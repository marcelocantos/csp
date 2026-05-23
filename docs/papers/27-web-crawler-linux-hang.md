# Paper 27 — `web_crawler` Linux-only deadlock

🎯 Target: T26
Status: **partially diagnosed — reproducible, root cause narrowed to runtime M:N
wake path, not the example.**
Filed: 2026-05-22, updated 2026-05-23 with docker-based repro findings.

## Symptom

`./build/normal/examples/web_crawler` invoked directly hangs and is killed by
the 60-second `gtimeout` in `make run-examples-ci` on Linux x86_64 and Linux
arm64. On macOS arm64 the same binary completes within ~1 s and prints
`Crawl complete: N pages fetched, M duplicates skipped.`

After 🎯T27's fix (PR #66, `has_hook_` gate on the quiescence wake), the
process no longer pegs CPU at 99% — it sleeps quietly until the timeout
fires.

## Reliable local repro

The hang reproduces under OrbStack on macOS arm64 with a clean
`ubuntu:24.04` container, matching CI's compiler exactly:

```sh
docker run --rm --platform linux/arm64 \
  -v "$PWD":/csp -w /csp ubuntu:24.04 bash -c '
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      clang-18 libc++-18-dev libc++abi-18-dev make
    make build/normal/examples/web_crawler \
      CC=clang-18 CXX="clang++-18 -std=c++20 -stdlib=libc++"
    timeout 8 ./build/normal/examples/web_crawler
  '
```

Hit rate is roughly 50% in a 10-run loop. The pattern in a hung run is
always the same: server starts, port-handshake succeeds, coordinator
prints the header, 1–4 worker lines appear, then nothing — the program
sleeps cleanly to the timeout.

## What's been ruled out

- **Not a TSan/ASan-detectable race**: the sanitizer test suites pass on
  both Linux platforms.
- **Not introduced by T27**: T27 changed the symptom from busy-spin to
  silent hang. The deadlock was already there.
- **Not at startup**: with `setbuf(stdout, nullptr)` and stderr
  checkpoints, the program clearly gets past `http::serve(0)`, port
  handshake, the first printf, the buffered-channel constructors, and
  the worker spawn loop. The hang is *mid-crawl*, after some forward
  progress.
- **Not the unbuffered `results` channel**: changing
  `chan<CrawlResult> results;` to `chan<CrawlResult> results(32);` had no
  effect on the failure rate (5/10 pass either way).
- **Not the rate-limit `sleep()`**: timing-only suspensions are not what
  hangs here — there is no scheduler tick waiting for time to advance.

## What it is NOT (added 2026-05-23)

### Negative result 1 — not the worker-wake granularity

Tried replacing `unpark_one()` with a brute-force wake-all variant — wake every parked / sleeping worker on every push instead of scanning for one. Result: **no improvement, still ~50% fail rate (9/20 pass).** Conclusion: the bug is not in the wake-exactly-one granularity. Workers are getting woken; they are not the bottleneck.

### Negative result 2 — not the contended-chan-op rendezvous protocol

Wrote `formal/ChanWriteContention.tla` modeling N writers (data senders) concurrently rendezvousing with 1 reader on an unbuffered channel, including the full `Imp::schedule` short-circuit ladder (`in_global_` / `suspending_` + `wake_pending_` / `in_local_`) and the `drain_suspended` clear/exchange/push cycle. Properties:

- `TypeOK` — domain invariants on all state.
- `AtMostOneMatch` — only one writer can be `alt_state = claimed` per reader scan.
- `SignalImpliesClaimed` — signal is only set on claimed writers.
- `NoLostWriter` — every claimed writer is eventually runnable (in_global / in_local / running) or has a pending action that will make it so.
- `EveryMatchedWriterCompletes` — liveness: every matched writer eventually reaches `pc_writer = done`.

TLC at 2 writers (440 states) and 3 writers (3,022 states): **all properties hold**. The companion `ChanWriteContention_Bug.tla` removes the `suspending_` check from the buggy `ReaderAltEndScheduleBuggy` action; TLC violates `NoScheduleDuringSuspend` in 60 states, confirming the spec is actually exercising what we think.

So the basic chan_op rendezvous protocol is formally verified correct for the contended single-channel case. The bug is **not** here.

What this leaves on the table:

- An imp that needs scheduling (probably the buffered-`frontier` filter imp,
  or a suspended worker/coordinator) is never getting `make_runnable()` /
  `schedule()` called on it at all — the wake never *fires* in the first
  place.
- Or a `chan_op` rendezvous loses state: one side believes it rendezvoused
  and continues; the other side stays suspended forever.

Both shapes are consistent with the heisenbug timing pattern (anything that
slows the contended-rendezvous window down eliminates the race).

## Remaining hypothesis after both negative results

The two negatives (wake-all granularity, single-channel chan_op rendezvous) leave the following candidates:

- **The buffered-`chan` filter imp's alt loop.** `chan<T>(N)` spawns a filter
  imp running `alt(read from in, write to out)` in a loop (see the
  body of `chan<T>::chan(size_t)` in `include/csp/csp.h`). Each alt
  completion re-suspends the filter on a fresh prialt. A missed
  re-arm here would manifest exactly as web_crawler's symptom — a
  filter that delivered one batch then never delivers again.
  **Modelling this is the next step.**
- **Multi-channel coordination** between workers' frontier-read,
  filter-deliver, and results-write. The protocol spans three imps
  and two channels per worker; a `make_runnable` race across channels
  is plausible.
- **A non-protocol-level issue** (memory ordering on a specific
  atomic, ABI difference between macOS and Linux libc++ futex
  implementations). Lower probability but cannot be ruled out without
  modelling the protocol layers exhaustively.

The heisenbug shape is consistent with all of these — slowing the
contended-rendezvous window (with syscalls or gdb) gives the missed
schedule call enough time to actually happen.

The smoking gun is the heisenbug shape:

- Adding any syscall in the coordinator path (`fflush`, `fprintf`,
  `setbuf(stdout, nullptr)`) shifts but does not eliminate the failure
  rate.
- Attaching `gdb` to the hung process — or even just installing `gdb` in
  the same container before running, which slightly perturbs scheduling
  — drops the hit rate to near zero.
- The bug appeared when CI started using `make run-examples-ci`
  (2026-05-12) but the underlying scheduler change predates it: the
  "per-worker-wake" refactor moved workers off `park_cv` onto per-worker
  `Note`s, and `csp.cc:594-599` calls out the resulting requirement that
  spawn must call `unpark_one()` explicitly in addition to notifying
  `park_cv`. If there is a window where a spawn-and-suspend interleaving
  pushes work to the global queue but neither wakes a worker via Note
  nor notifies `park_cv`, exactly this symptom results.

Relevant code paths to audit when picking this back up:

- `src/csp.cc:599` (spawn → `unpark_one`)
- `src/runtime.cpp:158-195` (`unpark_one` — first-pass sleeper scan,
  second-pass flag-awake, fallback `park_cv.notify_all`)
- `src/csp.cc:95-116` (`drain_suspended` → `push_to_global` →
  `unpark_one`)
- `src/csp.cc:206` (steal-then-push path)

The TLA spec covering this is `formal/PerWorkerWake.tla` plus the
recently-added `formal/WorkerJoin.tla` (T5). Re-running TLC with a
slightly enriched model — specifically, modelling the *transition*
between Note `SLEEPING` and `AWAKE` while spawn is mid-push — may turn
up the violating interleaving.

## Side discovery: Linux build portability bugs

While setting up the repro I had to add
`-DHAVE_ARPA_INET_H=1 -DHAVE_NETINET_IN_H=1` to `NGHTTP2_CFLAGS` and
`NGHTTP3_CFLAGS` in `Makefile`. Without those defines, a clean
`ubuntu:24.04` container fails to build with:

```
nghttp2_hd_huffman.c:65:11: error: call to undeclared function 'htonl'
nghttp3_conv.c:45:11: error: call to undeclared function 'ntohs'
```

CI happens to build because the GitHub `ubuntu-24.04` runner image
pre-installs additional dev packages whose headers transitively pull
in `arpa/inet.h`. A clean Debian/Ubuntu rootfs does not. The fix
matches what `wslay` and `ngtcp2` were already doing in the same
Makefile. This fix is included in this PR.

## Diagnostic plumbing in `examples/web_crawler.cc`

The example now does:

- `setbuf(stdout, nullptr)` at the top of `main()`, so worker `printf`s
  appear in real time under pipe-captured stdout (CI, `tee`, redirects).
- `fprintf(stderr, "[wc] …")` checkpoints at `main` entry, server entry
  before/after `http::serve`, server after `port_w << srv.port`,
  coordinator before/after `port_r >> port`, coordinator after
  `port_r = {}`.

These are intended to stay until the runtime bug is fixed; the noise
on the happy path is two server lines + two coordinator lines.

## What this PR ships

1. `Makefile`: `-DHAVE_ARPA_INET_H=1 -DHAVE_NETINET_IN_H=1` for both
   `NGHTTP2_CFLAGS` and `NGHTTP3_CFLAGS` — closes the clean-container
   build gap.
2. `examples/web_crawler.cc`: `setbuf` + diagnostic checkpoints (above).
3. This paper, with the docker repro + smoking-gun analysis.
4. `web_crawler` stays in `EXAMPLE_CI_SKIP` until the runtime bug is
   fixed.

🎯T26 stays open; acceptance now reads "Linux-only missed-wakeup race
identified — diagnose and fix in the M:N scheduler's per-worker Note
path."

## Next steps (for whoever picks this up)

1. **TLA**: extend `PerWorkerWake.tla` to model the spawn-while-park
   interleaving above. Run TLC; if it finds a violation, the trace
   names the fix.
2. **Instrumentation, not gdb**: gdb hides the bug. Use atomic counters
   in the runtime (`spawned`, `unpark_one_calls`, `note_wakes`,
   `park_cv_notifies`, `parked_workers_at_spawn`) and dump them on
   timeout via a SIGUSR1 handler — invasive enough to log, light
   enough to not perturb scheduling out of the bug.
3. **Repro reliably**: try increasing worker count, lowering the
   rate-limit gap, or adding an artificial yield in the spawn path
   to make the window larger. Goal: 100% repro rate.
4. **Once fixed**: remove `web_crawler` from `EXAMPLE_CI_SKIP` in
   `Makefile`, remove the diagnostic plumbing in `examples/web_crawler.cc`,
   and retire 🎯T26.
