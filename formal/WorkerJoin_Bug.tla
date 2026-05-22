---- MODULE WorkerJoin_Bug ----
(*******************************************************************************
 * Bug variant of WorkerJoin.
 *
 * Defect: shutdown reads num_procs_ exactly ONCE (no stability loop).
 * It wakes [1..N), then immediately moves to join the watchdog and then
 * join all workers [1..N).
 *
 * Race: the watchdog adds a new worker (increments num_procs_ to N+1) between
 * shutdown's single read and the watchdog join. Worker N+1 is spawned but
 * never woken. Since stopping=true, worker N+1 would eventually exit on its
 * own (it will see stopping on its next check_work iteration), but in this
 * model we verify the join safety property: shutdown tries to join workers
 * [1..N) only. Worker N+1 (w3) is neither woken by shutdown nor included in
 * the join set — so AllWorkersJoined is violated.
 *
 * Expected TLC result: invariant violation of AllWorkersJoined.
 *   TLC will find a state where shutdown_pc = "done" but w3 is not in joined
 *   (because w3 was added after the single read and never woken/joined).
 *
 * See also: docs/papers/26-mn-worker-join.md
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    InitialWorkers,
    SurplusWorkers,
    MaxProcs

Workers == InitialWorkers \cup SurplusWorkers

VARIABLES
    stopping,
    num_procs,
    worker_state,
    watchdog_pc,
    shutdown_pc,
    shutdown_n,         \* The single read of num_procs (no loop)
    joined

vars == <<stopping, num_procs, worker_state, watchdog_pc,
          shutdown_pc, shutdown_n, joined>>

Init ==
    /\ stopping = FALSE
    /\ num_procs = Cardinality(InitialWorkers)
    /\ worker_state = [w \in Workers |->
                           IF w \in InitialWorkers THEN "running" ELSE "not_started"]
    /\ watchdog_pc = "running"
    /\ shutdown_pc = "idle"
    /\ shutdown_n = 0
    /\ joined = {}

(*******************************************************************************
 * WATCHDOG ACTIONS (same as correct version)
 ******************************************************************************)

WatchdogCheckStop ==
    /\ watchdog_pc = "running"
    /\ stopping
    /\ watchdog_pc' = "exited"
    /\ UNCHANGED <<stopping, num_procs, worker_state, shutdown_pc,
                   shutdown_n, joined>>

WatchdogAddProc ==
    /\ watchdog_pc = "running"
    /\ ~stopping
    /\ num_procs < MaxProcs
    /\ \E w \in SurplusWorkers : worker_state[w] = "not_started"
    /\ LET w == CHOOSE sw \in SurplusWorkers : worker_state[sw] = "not_started"
       IN /\ num_procs' = num_procs + 1
          /\ worker_state' = [worker_state EXCEPT ![w] = "running"]
    /\ UNCHANGED <<stopping, watchdog_pc, shutdown_pc, shutdown_n, joined>>

(*******************************************************************************
 * WORKER ACTIONS
 ******************************************************************************)

WorkerExit(w) ==
    /\ worker_state[w] = "woken"
    /\ stopping
    /\ worker_state' = [worker_state EXCEPT ![w] = "exited"]
    /\ UNCHANGED <<stopping, num_procs, watchdog_pc, shutdown_pc,
                   shutdown_n, joined>>

(*******************************************************************************
 * SHUTDOWN ACTIONS (BUG: no stability loop — single read of num_procs_)
 ******************************************************************************)

ShutdownSetStop ==
    /\ shutdown_pc = "idle"
    /\ stopping' = TRUE
    /\ shutdown_pc' = "single_read"
    /\ UNCHANGED <<num_procs, worker_state, watchdog_pc, shutdown_n, joined>>

(* BUG: Read num_procs_ exactly once.
 *
 * TLA:WorkerJoin_Bug.ShutdownSingleRead *)
ShutdownSingleRead ==
    /\ shutdown_pc = "single_read"
    /\ shutdown_n' = num_procs
    /\ shutdown_pc' = "wake_range"
    /\ UNCHANGED <<stopping, num_procs, worker_state, watchdog_pc, joined>>

(* Wake only the workers that were counted in the single read.
 * Workers added after the read (surplus w3) are NOT woken here.
 *
 * TLA:WorkerJoin_Bug.ShutdownWakeRange *)
ShutdownWakeRange ==
    /\ shutdown_pc = "wake_range"
    \* BUG: only wake workers with ordinal <= shutdown_n.
    \* We model this as: only wake InitialWorkers (whose slot is always <= N).
    \* SurplusWorkers that were added after the read are NOT woken.
    /\ worker_state' = [w \in Workers |->
                           IF w \in InitialWorkers /\ worker_state[w] = "running"
                           THEN "woken"
                           ELSE worker_state[w]]
    /\ shutdown_pc' = "join_watchdog"
    /\ UNCHANGED <<stopping, num_procs, watchdog_pc, shutdown_n, joined>>

(* BUG: Join watchdog without re-checking num_procs.
 * At this point the watchdog may have added w3 between ShutdownSingleRead
 * and now. After watchdog joins, w3 is running but will never be explicitly
 * joined by shutdown (it only joins [1..shutdown_n)). *)
ShutdownJoinWatchdog ==
    /\ shutdown_pc = "join_watchdog"
    /\ watchdog_pc = "exited"
    /\ shutdown_pc' = "join_all"
    /\ UNCHANGED <<stopping, num_procs, worker_state, watchdog_pc,
                   shutdown_n, joined>>

(* BUG: Join only workers [1..shutdown_n). Workers added after the read
 * (i.e., SurplusWorkers that started after the read) are not joined. *)
ShutdownJoinOne ==
    /\ shutdown_pc = "join_all"
    /\ \E w \in InitialWorkers :    \* BUG: only join initial workers
           /\ worker_state[w] = "exited"
           /\ w \notin joined
           /\ joined' = joined \cup {w}
    /\ UNCHANGED <<stopping, num_procs, worker_state, watchdog_pc,
                   shutdown_pc, shutdown_n>>

(* Shutdown declares done once all INITIAL workers are joined.
 * Surplus workers (w3) are silently skipped — the bug. *)
ShutdownDone ==
    /\ shutdown_pc = "join_all"
    /\ \A w \in InitialWorkers : w \in joined   \* BUG: only checks initial
    /\ shutdown_pc' = "done"
    /\ UNCHANGED <<stopping, num_procs, worker_state, watchdog_pc,
                   shutdown_n, joined>>

Next ==
    \/ WatchdogCheckStop
    \/ WatchdogAddProc
    \/ \E w \in Workers : WorkerExit(w)
    \/ ShutdownSetStop
    \/ ShutdownSingleRead
    \/ ShutdownWakeRange
    \/ ShutdownJoinWatchdog
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
    /\ shutdown_pc \in {"idle", "single_read", "wake_range",
                        "join_watchdog", "join_all", "done"}
    /\ shutdown_n \in 0..MaxProcs
    /\ joined \subseteq Workers

(* This WILL be violated: shutdown declares done without joining w3 (surplus
 * worker added after the single read). *)
AllWorkersJoined ==
    shutdown_pc = "done" =>
        \A w \in Workers :
            worker_state[w] = "not_started" \/ w \in joined

====
