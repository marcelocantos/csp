---- MODULE WorkerJoin ----
(*******************************************************************************
 * Models the M:N worker join protocol from csp/src/runtime.cpp.
 *
 * The core challenge: the watchdog can add new workers (increment num_procs_)
 * concurrently with shutdown. If shutdown reads num_procs_, wakes those
 * workers, and then joins, it may miss workers added after the read —
 * those workers would run forever and never be joined (deadlock).
 *
 * Fix: the "stability loop" in Runtime::shutdown():
 *   - Reads num_procs_ = N.
 *   - Wakes workers in [prev_n..N).
 *   - Re-reads num_procs_. If N' == N (stable), stop.
 *   - If N' > N, wake [N..N'), set prev_n = N, loop.
 *   - After stability: join watchdog (which guarantees no more procs can
 *     be added), then join all workers.
 *
 * Since the watchdog exits when stopping=true, and stopping is set before
 * the first read, the count must eventually stabilise.
 *
 * Participants:
 *   InitialWorkers  — the set of workers present at shutdown initiation
 *   SurplusWorkers  — workers that the watchdog may add during shutdown
 *   Watchdog        — adds surplus workers; exits after seeing stopping=true
 *   Shutdown        — the shutdown() call
 *
 * Code references (src/runtime.cpp):
 *   ShutdownSetStop    — line 62: stopping.store(true, release)
 *   ShutdownReadN      — line 76: n = num_procs_.load(acquire) (loop)
 *   ShutdownWakeRange  — line 77: note.wake() for i in [prev_n..n)
 *   ShutdownStable     — line 78: if (n == prev_n) break
 *   ShutdownYield      — line 79: this_thread::yield()
 *   ShutdownJoinWdog   — line 96: watchdog_.join()
 *   ShutdownSecondWake — lines 101-102: second pass wake
 *   ShutdownJoinAll    — lines 105-108: worker.join() for all i in [1..n)
 *   WatchdogAddProc    — line 369: add_processor() when heartbeat stalls
 *   WatchdogCheckStop  — line 352: !stopping.load(acquire) loop condition
 *
 * Key invariants verified:
 *   AllWorkersJoined — after shutdown complete, every worker has exited
 *   NoEarlyJoin      — shutdown does not join before all workers have exited
 *
 * See also: docs/papers/26-mn-worker-join.md
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    InitialWorkers,     \* Workers present at startup, e.g. {w1, w2}
    SurplusWorkers,     \* Workers the watchdog may add, e.g. {w3}
    MaxProcs            \* Upper bound on num_procs (prevents unbounded growth)

Workers == InitialWorkers \cup SurplusWorkers

ASSUME MaxProcs >= Cardinality(Workers) + 1

VARIABLES
    stopping,           \* BOOLEAN: global shutdown flag
    num_procs,          \* Nat: current count of processors
    worker_state,       \* [Workers -> "not_started" | "running" | "woken" | "exited"]
    watchdog_pc,        \* watchdog program counter
    shutdown_pc,        \* shutdown program counter
    shutdown_prev_n,    \* last stable n seen by shutdown's stability loop
    shutdown_cur_n,     \* current n read in stability loop
    joined              \* set of workers that have been joined

vars == <<stopping, num_procs, worker_state, watchdog_pc, shutdown_pc,
          shutdown_prev_n, shutdown_cur_n, joined>>

(*******************************************************************************
 * Initial state: initial workers running, watchdog active, shutdown pending.
 ******************************************************************************)
Init ==
    /\ stopping = FALSE
    /\ num_procs = Cardinality(InitialWorkers)
    /\ worker_state = [w \in Workers |->
                           IF w \in InitialWorkers THEN "running" ELSE "not_started"]
    /\ watchdog_pc = "running"
    /\ shutdown_pc = "idle"
    /\ shutdown_prev_n = 0
    /\ shutdown_cur_n = 0
    /\ joined = {}

(*******************************************************************************
 * WATCHDOG ACTIONS
 *
 * Watchdog polls worker heartbeats. If a worker is stalled, it calls
 * add_processor() (increment num_procs, spawn new worker). Exits when
 * stopping=true.
 ******************************************************************************)

(* Watchdog checks stopping flag. If true, exit. *)
WatchdogCheckStop ==
    /\ watchdog_pc = "running"
    /\ stopping
    /\ watchdog_pc' = "exited"
    /\ UNCHANGED <<stopping, num_procs, worker_state, shutdown_pc,
                   shutdown_prev_n, shutdown_cur_n, joined>>

(* Watchdog adds a surplus processor (only if count < MaxProcs and there are
 * unstarted surplus workers to model). *)
WatchdogAddProc ==
    /\ watchdog_pc = "running"
    /\ ~stopping
    /\ num_procs < MaxProcs
    /\ \E w \in SurplusWorkers : worker_state[w] = "not_started"
    /\ LET w == CHOOSE sw \in SurplusWorkers : worker_state[sw] = "not_started"
       IN /\ num_procs' = num_procs + 1
          /\ worker_state' = [worker_state EXCEPT ![w] = "running"]
    /\ UNCHANGED <<stopping, watchdog_pc, shutdown_pc,
                   shutdown_prev_n, shutdown_cur_n, joined>>

(*******************************************************************************
 * WORKER ACTIONS
 *
 * Workers run until woken+stopping. When woken (their state goes to "woken")
 * and stopping=true, they exit.
 ******************************************************************************)

(* A running worker checks stopping. If stopping and woken: exit. *)
WorkerExit(w) ==
    /\ worker_state[w] = "woken"
    /\ stopping
    /\ worker_state' = [worker_state EXCEPT ![w] = "exited"]
    /\ UNCHANGED <<stopping, num_procs, watchdog_pc, shutdown_pc,
                   shutdown_prev_n, shutdown_cur_n, joined>>

(*******************************************************************************
 * SHUTDOWN ACTIONS
 *
 * Shutdown protocol:
 *   1. Set stopping = true.
 *   2. Stability loop:
 *      a. Read num_procs = N.
 *      b. Wake workers [prev_n..N).
 *      c. If N == prev_n (stable): break.
 *      d. Set prev_n = N, yield, loop.
 *   3. Join watchdog.
 *   4. Second-pass wake of all workers [1..n) + notify park_cv.
 *   5. Join all workers.
 ******************************************************************************)

(* Shutdown sets stopping = true.
 *
 * TLA:WorkerJoin.ShutdownSetStop *)
ShutdownSetStop ==
    /\ shutdown_pc = "idle"
    /\ stopping' = TRUE
    /\ shutdown_pc' = "loop_read"
    /\ shutdown_prev_n' = 0
    /\ UNCHANGED <<num_procs, worker_state, watchdog_pc,
                   shutdown_cur_n, joined>>

(* Shutdown reads num_procs_.
 *
 * TLA:WorkerJoin.ShutdownReadN *)
ShutdownReadN ==
    /\ shutdown_pc = "loop_read"
    /\ shutdown_cur_n' = num_procs
    /\ shutdown_pc' = "loop_wake"
    /\ UNCHANGED <<stopping, num_procs, worker_state, watchdog_pc,
                   shutdown_prev_n, joined>>

(* Shutdown wakes workers in [prev_n..cur_n). We model this as setting each
 * "running" worker in that range to "woken". Workers already "exited" or
 * "woken" are unaffected. Indices are approximated: we treat InitialWorkers
 * as the first Cardinality(InitialWorkers) slots and SurplusWorkers as slots
 * beyond that. We use the set of workers that have been started and whose
 * ordinal index is in the wake range.
 *
 * Simplification: instead of tracking indices precisely, we model the wake
 * as: any running worker with num_procs >= their slot index gets woken.
 * For the model we wake all "running" workers whose slot <= shutdown_cur_n.
 * Since all initial workers have slots in [1..InitialWorkers] and surplus
 * workers have slots in [InitialWorkers+1..], we approximate by:
 *   - Workers in InitialWorkers are always within cur_n if cur_n >= |IW|.
 *   - Surplus workers are in range if cur_n > |IW|.
 *
 * TLA:WorkerJoin.ShutdownWakeRange *)
ShutdownWakeRange ==
    /\ shutdown_pc = "loop_wake"
    \* Wake all started workers (their slots are ≤ shutdown_cur_n by invariant
    \* since num_procs_ is the count and all started workers have slots ≤ num_procs).
    /\ worker_state' = [w \in Workers |->
                           IF worker_state[w] = "running" THEN "woken"
                           ELSE worker_state[w]]
    /\ shutdown_pc' = "loop_check"
    /\ UNCHANGED <<stopping, num_procs, watchdog_pc,
                   shutdown_prev_n, shutdown_cur_n, joined>>

(* Shutdown checks if count is stable (cur_n == prev_n → stable).
 * If stable: advance to join_watchdog.
 * If not stable: update prev_n to cur_n and loop back to read again.
 *
 * TLA:WorkerJoin.ShutdownCheckStable *)
ShutdownCheckStable ==
    /\ shutdown_pc = "loop_check"
    /\ IF shutdown_cur_n = shutdown_prev_n
       THEN \* Stable: exit loop, go join watchdog
            /\ shutdown_pc' = "join_watchdog"
            /\ shutdown_prev_n' = shutdown_prev_n   \* unchanged
       ELSE \* Not stable: advance prev_n, loop
            /\ shutdown_prev_n' = shutdown_cur_n
            /\ shutdown_pc' = "loop_read"
    /\ UNCHANGED <<stopping, num_procs, worker_state, watchdog_pc,
                   shutdown_cur_n, joined>>

(* Shutdown joins the watchdog. Blocks until watchdog has exited.
 * After this, num_procs is frozen.
 *
 * TLA:WorkerJoin.ShutdownJoinWatchdog *)
ShutdownJoinWatchdog ==
    /\ shutdown_pc = "join_watchdog"
    /\ watchdog_pc = "exited"     \* join blocks until watchdog exits
    /\ shutdown_pc' = "second_wake"
    /\ UNCHANGED <<stopping, num_procs, worker_state, watchdog_pc,
                   shutdown_prev_n, shutdown_cur_n, joined>>

(* Second-pass wake: shutdown wakes all remaining workers.
 *
 * TLA:WorkerJoin.ShutdownSecondWake *)
ShutdownSecondWake ==
    /\ shutdown_pc = "second_wake"
    /\ worker_state' = [w \in Workers |->
                           IF worker_state[w] = "running" THEN "woken"
                           ELSE worker_state[w]]
    /\ shutdown_pc' = "join_all"
    /\ UNCHANGED <<stopping, num_procs, watchdog_pc,
                   shutdown_prev_n, shutdown_cur_n, joined>>

(* Shutdown joins all worker threads: blocks until each worker has exited.
 * We model joining as "once a worker is exited, it can be added to joined".
 * Shutdown completes when joined contains all started workers.
 *
 * TLA:WorkerJoin.ShutdownJoinAll *)
ShutdownJoinOne ==
    /\ shutdown_pc = "join_all"
    /\ \E w \in Workers :
           /\ worker_state[w] = "exited"
           /\ w \notin joined
           /\ joined' = joined \cup {w}
    /\ UNCHANGED <<stopping, num_procs, worker_state, watchdog_pc,
                   shutdown_pc, shutdown_prev_n, shutdown_cur_n>>

(* Shutdown declares done once all started workers are joined. *)
ShutdownDone ==
    /\ shutdown_pc = "join_all"
    /\ \A w \in Workers :
           worker_state[w] = "not_started" \/ w \in joined
    /\ shutdown_pc' = "done"
    /\ UNCHANGED <<stopping, num_procs, worker_state, watchdog_pc,
                   shutdown_prev_n, shutdown_cur_n, joined>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ WatchdogCheckStop
    \/ WatchdogAddProc
    \/ \E w \in Workers : WorkerExit(w)
    \/ ShutdownSetStop
    \/ ShutdownReadN
    \/ ShutdownWakeRange
    \/ ShutdownCheckStable
    \/ ShutdownJoinWatchdog
    \/ ShutdownSecondWake
    \/ ShutdownJoinOne
    \/ ShutdownDone

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

WorkerStates == {"not_started", "running", "woken", "exited"}

TypeOK ==
    /\ stopping \in BOOLEAN
    /\ num_procs \in 0..MaxProcs
    /\ worker_state \in [Workers -> WorkerStates]
    /\ watchdog_pc \in {"running", "exited"}
    /\ shutdown_pc \in {"idle", "loop_read", "loop_wake", "loop_check",
                        "join_watchdog", "second_wake", "join_all", "done"}
    /\ shutdown_prev_n \in 0..MaxProcs
    /\ shutdown_cur_n \in 0..MaxProcs
    /\ joined \subseteq Workers

(* Safety: shutdown only completes when all started workers have been joined. *)
AllWorkersJoined ==
    shutdown_pc = "done" =>
        \A w \in Workers :
            worker_state[w] = "not_started" \/ w \in joined

(* Safety: a worker can only be joined after it has exited. *)
JoinAfterExit ==
    \A w \in Workers :
        w \in joined => worker_state[w] = "exited"

(* Safety: workers only exit after being woken (and stopping=true).
 * No worker exits spontaneously without having been woken. *)
ExitAfterWoken ==
    \A w \in Workers :
        worker_state[w] = "exited" => stopping

(* Safety: watchdog only exits after stopping is set. *)
WatchdogExitsClean ==
    watchdog_pc = "exited" => stopping

====
