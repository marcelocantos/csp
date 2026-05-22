# Paper 25: M:N Worker Join — Stability Loop and Watchdog Race

## Context

`Runtime::shutdown()` must drain and join all worker threads including surplus
workers that the watchdog may have added just before or during shutdown. The
hazard is that the watchdog adds a new `Processor` (increments `num_procs_`)
after shutdown has already finished waking workers — the new worker thread
would never be joined.

## Actors

1. **Shutdown** — calls `Runtime::shutdown()`. Iterates the stability loop:
   reads `num_procs_`, wakes workers [prev_n..n), compares with previous n.
   If stable (n == prev_n): stop the loop, then wait for watchdog to join,
   then do a second-pass wake + join.
2. **Watchdog** — runs `watchdog_loop()`. Polls worker heartbeats. When it
   sees a stalled worker, calls `add_processor()` which increments `num_procs_`
   under `global_mu`. Exits when `stopping == true`.
3. **Worker i** — runs `worker_loop()`. Exits when it sees `stopping == true`.

## Shutdown transition sequence

1. Shutdown stores `stopping = true` (release).
2. Shutdown reads `num_procs_ = N`. Wakes workers [1..N) via `note.wake()`.
3. Shutdown re-reads `num_procs_`. If watchdog has since added workers
   (N' > N), re-wake workers [N..N'). Repeat until stable.
4. Shutdown yields (`std::this_thread::yield()`) between iterations to let the
   watchdog see `stopping` and exit.
5. Shutdown joins the watchdog thread. After watchdog joins, `num_procs_` is
   frozen — no more procs can be added.
6. Shutdown does a final wake of workers [1..n) and a `park_cv.notify_all()`.
7. Shutdown joins all worker threads [1..n).

## Hazard hypothesis

**Without the stability loop**, the race is:

1. Shutdown reads `num_procs_ = N`. Wakes [1..N).
2. Watchdog adds worker N (increments `num_procs_` to N+1, spawns thread N).
3. Shutdown checks count again: still N (read happened before the increment).
   Stability is declared incorrectly. Watchdog is now joined (watchdog exited
   separately), but worker N was never woken.
4. Worker N never sees `stopping` and never exits.
5. `procs[N]->worker.join()` deadlocks.

**With the stability loop** this is closed: the loop continues until two
successive reads of `num_procs_` agree. Since the watchdog exits after seeing
`stopping = true`, and `stopping` is set before the first read, the watchdog
will eventually stop adding procs and the count will stabilise.

## Invariant to verify

`Shutdown complete ⟹ all workers have exited`

Formally: `pc_shutdown = "joined_all" ⟹ ∀i ∈ Workers: pc_worker[i] = "exited"`

## Bug variant

Remove the stability loop. Shutdown reads `num_procs_` once, wakes [1..N),
immediately joins the watchdog, then joins [1..N). The watchdog may add worker
N between the single read and the watchdog join, spawning a thread that is
never woken and never joined.
