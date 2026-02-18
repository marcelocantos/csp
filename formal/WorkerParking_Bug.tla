---- MODULE WorkerParking_Bug ----
(*******************************************************************************
 * BUGGY VERSION — demonstrates the shutdown CV race (bug #8).
 *
 * In this version, shutdown does NOT acquire park_mu before notify_all.
 * This allows the following dangerous interleaving:
 *
 *   Worker: acquires park_mu
 *   Worker: evaluates CV predicate -> stopping=false (decides to wait)
 *   Shutdown: sets stopping=true (no lock needed, atomic store)
 *   Shutdown: calls notify_all — but worker hasn't entered cv.wait yet!
 *             (worker is in "will_wait" state, still holds park_mu)
 *   Worker: calls cv.wait(lk) — atomically releases lock + blocks
 *   Worker: stuck in "blocked" forever — notification was lost!
 *
 * With the fix: shutdown would block on park_mu (worker holds it),
 * worker would enter cv.wait (releasing park_mu), shutdown would then
 * acquire+release+notify, finding the worker blocked.
 *
 * Expected result: TLC finds a NoLostShutdown violation.
 ******************************************************************************)

EXTENDS Integers

CONSTANTS
    Workers

VARIABLES
    stopping,
    park_mu,
    woken,
    pc_worker,
    pc_shutdown

vars == <<stopping, park_mu, woken, pc_worker, pc_shutdown>>

Init ==
    /\ stopping = FALSE
    /\ park_mu = "none"
    /\ woken = [w \in Workers |-> FALSE]
    /\ pc_worker = [w \in Workers |-> "check_work"]
    /\ pc_shutdown = "idle"

(*******************************************************************************
 * WORKER ACTIONS — identical to the correct version.
 ******************************************************************************)

WorkerCheckWork(w) ==
    /\ pc_worker[w] = "check_work"
    /\ IF stopping
       THEN /\ pc_worker' = [pc_worker EXCEPT ![w] = "exited"]
            /\ UNCHANGED <<stopping, park_mu, woken, pc_shutdown>>
       ELSE /\ pc_worker' = [pc_worker EXCEPT ![w] = "acquire_park"]
            /\ UNCHANGED <<stopping, park_mu, woken, pc_shutdown>>

WorkerAcquirePark(w) ==
    /\ pc_worker[w] = "acquire_park"
    /\ park_mu = "none"
    /\ park_mu' = w
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "eval_pred"]
    /\ UNCHANGED <<stopping, woken, pc_shutdown>>

WorkerEvalPred(w) ==
    /\ pc_worker[w] = "eval_pred"
    /\ park_mu = w
    /\ IF stopping
       THEN /\ pc_worker' = [pc_worker EXCEPT ![w] = "wake"]
            /\ UNCHANGED <<stopping, park_mu, woken, pc_shutdown>>
       ELSE /\ pc_worker' = [pc_worker EXCEPT ![w] = "will_wait"]
            /\ UNCHANGED <<stopping, park_mu, woken, pc_shutdown>>

(* cv.wait(lk): atomically release lock + block *)
WorkerEnterWait(w) ==
    /\ pc_worker[w] = "will_wait"
    /\ park_mu = w
    /\ park_mu' = "none"
    /\ woken' = [woken EXCEPT ![w] = FALSE]
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "blocked"]
    /\ UNCHANGED <<stopping, pc_shutdown>>

WorkerWoken(w) ==
    /\ pc_worker[w] = "blocked"
    /\ woken[w]
    /\ park_mu = "none"
    /\ park_mu' = w
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "eval_pred"]
    /\ UNCHANGED <<stopping, woken, pc_shutdown>>

WorkerWake(w) ==
    /\ pc_worker[w] = "wake"
    /\ park_mu = w
    /\ park_mu' = "none"
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "check_work"]
    /\ UNCHANGED <<stopping, woken, pc_shutdown>>

(*******************************************************************************
 * SHUTDOWN ACTIONS — BUG: no park_mu acquire before notify.
 *
 * Without the empty critical section on park_mu, shutdown can set
 * the flag and call notify_all while a worker is in "will_wait" state
 * (has evaluated pred as false, still holds park_mu, hasn't entered
 * cv.wait yet). The notification targets only currently-blocked
 * workers, so this worker misses it.
 ******************************************************************************)

(* Shutdown sets the stopping flag.
 * Code: stopping.store(true, release)  [line 62] *)
ShutdownSetFlag ==
    /\ pc_shutdown = "idle"
    /\ stopping' = TRUE
    /\ pc_shutdown' = "notify"      \* BUG: skip directly to notify
    /\ UNCHANGED <<park_mu, woken, pc_worker>>

(* Shutdown calls notify_all WITHOUT acquiring park_mu first.
 * Only workers currently in "blocked" state receive the wake signal.
 * Code (buggy): park_cv.notify_all() without prior lock_guard *)
ShutdownNotify ==
    /\ pc_shutdown = "notify"
    /\ woken' = [w \in Workers |->
                    IF pc_worker[w] = "blocked" THEN TRUE ELSE woken[w]]
    /\ pc_shutdown' = "done"
    /\ UNCHANGED <<stopping, park_mu, pc_worker>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ \E w \in Workers :
        \/ WorkerCheckWork(w)
        \/ WorkerAcquirePark(w)
        \/ WorkerEvalPred(w)
        \/ WorkerEnterWait(w)
        \/ WorkerWoken(w)
        \/ WorkerWake(w)
    \/ ShutdownSetFlag
    \/ ShutdownNotify

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ stopping \in BOOLEAN
    /\ park_mu \in ({"none", "shutdown"} \cup Workers)
    /\ woken \in [Workers -> BOOLEAN]
    /\ pc_worker \in [Workers -> {"check_work", "acquire_park", "eval_pred",
                                   "will_wait", "blocked", "wake", "exited"}]
    /\ pc_shutdown \in {"idle", "notify", "done"}

NoLostShutdown ==
    (pc_shutdown = "done") =>
        \A w \in Workers : pc_worker[w] = "blocked" => woken[w]

NoSpuriousExit ==
    \A w \in Workers : pc_worker[w] = "exited" => stopping

====
