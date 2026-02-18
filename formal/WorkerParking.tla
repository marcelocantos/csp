---- MODULE WorkerParking ----
(*******************************************************************************
 * Models the park/unpark/shutdown protocol from csp/src/runtime.cpp.
 *
 * The protocol ensures that when shutdown sets the stopping flag and
 * notifies park_cv, no worker gets stuck in wait() — even if a worker
 * has already evaluated the CV predicate as false but hasn't yet
 * entered the internal cv.wait(lk) blocking call.
 *
 * Participants:
 *   W1, W2    — two worker threads running worker_loop
 *   Shutdown  — the shutdown() call
 *
 * Code references (src/runtime.cpp):
 *   WorkerCheckWork    — line 102: while (!stopping) { ... }
 *   WorkerAcquirePark  — line 127: unique_lock<mutex> lk(park_mu)
 *   WorkerEvalPred     — park_cv.wait predicate evaluation
 *   WorkerEnterWait    — cv.wait(lk): atomically release lock + block
 *   WorkerWoken        — cv wakes: reacquire lock, recheck pred
 *   WorkerWake         — line 156: p.parked.store(false)
 *   ShutdownSetFlag    — line 62: stopping.store(true)
 *   ShutdownAcquireMu  — line 68: lock_guard<mutex> lk(park_mu)
 *   ShutdownReleaseMu  — line 68: } (end of lock_guard scope)
 *   ShutdownNotify     — line 69: park_cv.notify_all()
 *
 * CV semantics:
 *   park_cv.wait(lk, pred) expands to: while(!pred()) { cv.wait(lk); }
 *   - pred() is evaluated with the lock held
 *   - cv.wait(lk) ATOMICALLY releases the lock and blocks
 *   - On wake: reacquire lock, recheck pred
 *   - The gap between "pred returns false" and "cv.wait(lk) blocks" is
 *     where the lock is still held but the thread hasn't entered wait.
 *     notify_all during this gap is lost for this thread.
 *
 * The fix (empty critical section): shutdown acquires park_mu before
 * notify_all. If a worker is between eval_pred and enter_wait (lock held),
 * shutdown blocks on park_mu. The worker then enters cv.wait, releasing
 * the lock. Shutdown acquires, releases, then notifies — finding the
 * worker blocked.
 ******************************************************************************)

EXTENDS Integers

CONSTANTS
    Workers     \* set of worker IDs, e.g. {w1, w2}

VARIABLES
    stopping,       \* Boolean: global shutdown flag
    park_mu,        \* "none" | worker ID | "shutdown" — who holds park_mu
    woken,          \* [Workers -> BOOLEAN]: per-worker CV wake signal
    pc_worker,      \* [Workers -> pc state]: per-worker program counter
    pc_shutdown     \* shutdown thread program counter

vars == <<stopping, park_mu, woken, pc_worker, pc_shutdown>>

(*******************************************************************************
 * Initial state: all workers are in their work loop, no shutdown in progress.
 ******************************************************************************)
Init ==
    /\ stopping = FALSE
    /\ park_mu = "none"
    /\ woken = [w \in Workers |-> FALSE]
    /\ pc_worker = [w \in Workers |-> "check_work"]
    /\ pc_shutdown = "idle"

(*******************************************************************************
 * WORKER ACTIONS
 *
 * Worker phases:
 *   check_work -> acquire_park -> eval_pred -> enter_wait -> blocked
 *                                           -> wake (if pred true)
 *   blocked -> woken (reacquire lock) -> eval_pred (recheck)
 ******************************************************************************)

(* Worker checks for work at top of loop. If stopping, exit.
 * Otherwise proceed to park (we abstract away work availability since
 * the bug is about shutdown, not work distribution).
 *
 * Code: while (!stopping.load(acquire)) { ... if no work: park } *)
WorkerCheckWork(w) ==
    /\ pc_worker[w] = "check_work"
    /\ IF stopping
       THEN /\ pc_worker' = [pc_worker EXCEPT ![w] = "exited"]
            /\ UNCHANGED <<stopping, park_mu, woken, pc_shutdown>>
       ELSE /\ pc_worker' = [pc_worker EXCEPT ![w] = "acquire_park"]
            /\ UNCHANGED <<stopping, park_mu, woken, pc_shutdown>>

(* Worker acquires park_mu to enter the cv.wait region.
 *
 * Code: unique_lock<mutex> lk(park_mu)  [line 127] *)
WorkerAcquirePark(w) ==
    /\ pc_worker[w] = "acquire_park"
    /\ park_mu = "none"
    /\ park_mu' = w
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "eval_pred"]
    /\ UNCHANGED <<stopping, woken, pc_shutdown>>

(* Worker evaluates the cv.wait predicate with lock held.
 * This is the pred() call inside: while(!pred()) { cv.wait(lk); }
 *
 * If stopping is true: predicate is true, skip wait, proceed to wake.
 * If stopping is false: predicate is false, will enter cv.wait.
 *
 * The lock is STILL HELD after this step — the worker hasn't entered
 * cv.wait(lk) yet. This is the critical gap where the bug manifests.
 *
 * Code: [this, &p]{ return stopping.load(acquire) || has_work(p); } *)
WorkerEvalPred(w) ==
    /\ pc_worker[w] = "eval_pred"
    /\ park_mu = w
    /\ IF stopping
       THEN \* Predicate true: skip wait, proceed to wake
            /\ pc_worker' = [pc_worker EXCEPT ![w] = "wake"]
            /\ UNCHANGED <<stopping, park_mu, woken, pc_shutdown>>
       ELSE \* Predicate false: will enter cv.wait next
            /\ pc_worker' = [pc_worker EXCEPT ![w] = "will_wait"]
            /\ UNCHANGED <<stopping, park_mu, woken, pc_shutdown>>

(* Worker calls cv.wait(lk): ATOMICALLY release lock and block.
 * This is one TLA+ action because the lock release and blocking are
 * indivisible (pthread_cond_wait guarantee). After this step, the
 * worker does NOT hold park_mu and is in "blocked" state.
 *
 * We clear the worker's woken flag here — any stale wakeup from
 * a previous cycle is consumed.
 *
 * Code: cv.wait(lk) inside park_cv.wait(lk, pred) *)
WorkerEnterWait(w) ==
    /\ pc_worker[w] = "will_wait"
    /\ park_mu = w
    /\ park_mu' = "none"
    /\ woken' = [woken EXCEPT ![w] = FALSE]
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "blocked"]
    /\ UNCHANGED <<stopping, pc_shutdown>>

(* Worker is blocked on park_cv. When woken (by notify), reacquire lock.
 *
 * Code: cv wakes -> pthread_cond_wait reacquires mutex *)
WorkerWoken(w) ==
    /\ pc_worker[w] = "blocked"
    /\ woken[w]
    /\ park_mu = "none"
    /\ park_mu' = w
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "eval_pred"]
    /\ UNCHANGED <<stopping, woken, pc_shutdown>>

(* Worker wakes up (pred was true): release lock, back to work loop.
 *
 * Code: park_cv.wait returns; p.parked.store(false); } [line 156] *)
WorkerWake(w) ==
    /\ pc_worker[w] = "wake"
    /\ park_mu = w
    /\ park_mu' = "none"
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "check_work"]
    /\ UNCHANGED <<stopping, woken, pc_shutdown>>

(*******************************************************************************
 * SHUTDOWN ACTIONS
 *
 * Shutdown phases:
 *   idle -> set_flag -> acquire_mu -> release_mu -> notify -> done
 *
 * The critical fix (bug #8): acquire park_mu as an empty critical section
 * BEFORE calling notify_all. This ensures that if a worker is between
 * eval_pred (saw stopping=false) and enter_wait (hasn't blocked yet),
 * shutdown is forced to wait. The worker proceeds to enter_wait (releasing
 * park_mu and blocking). THEN shutdown acquires, releases, and notifies —
 * finding the worker blocked.
 ******************************************************************************)

(* Shutdown sets the stopping flag.
 *
 * Code: stopping.store(true, release)  [line 62] *)
ShutdownSetFlag ==
    /\ pc_shutdown = "idle"
    /\ stopping' = TRUE
    /\ pc_shutdown' = "acquire_mu"
    /\ UNCHANGED <<park_mu, woken, pc_worker>>

(* Shutdown acquires park_mu (empty critical section).
 *
 * Code: { lock_guard<mutex> lk(park_mu); }  [line 68] *)
ShutdownAcquireMu ==
    /\ pc_shutdown = "acquire_mu"
    /\ park_mu = "none"
    /\ park_mu' = "shutdown"
    /\ pc_shutdown' = "release_mu"
    /\ UNCHANGED <<stopping, woken, pc_worker>>

(* Shutdown releases park_mu.
 *
 * Code: } // end of lock_guard scope  [line 68] *)
ShutdownReleaseMu ==
    /\ pc_shutdown = "release_mu"
    /\ park_mu = "shutdown"
    /\ park_mu' = "none"
    /\ pc_shutdown' = "notify"
    /\ UNCHANGED <<stopping, woken, pc_worker>>

(* Shutdown calls notify_all on park_cv.
 * Only workers currently in "blocked" state receive the wake signal.
 * This correctly models CV notify semantics: only waiting threads are woken.
 *
 * Code: park_cv.notify_all()  [line 69] *)
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
    \/ ShutdownAcquireMu
    \/ ShutdownReleaseMu
    \/ ShutdownNotify

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

(* Type invariant: variables stay in their expected domains. *)
TypeOK ==
    /\ stopping \in BOOLEAN
    /\ park_mu \in ({"none", "shutdown"} \cup Workers)
    /\ woken \in [Workers -> BOOLEAN]
    /\ pc_worker \in [Workers -> {"check_work", "acquire_park", "eval_pred",
                                   "will_wait", "blocked", "wake", "exited"}]
    /\ pc_shutdown \in {"idle", "acquire_mu", "release_mu", "notify", "done"}

(* Safety: after shutdown completes (flag set + notify done), no worker is
 * stuck in "blocked" without a pending wakeup signal. Since stopping=TRUE,
 * any woken worker will see the predicate as true and exit. *)
NoLostShutdown ==
    (pc_shutdown = "done") =>
        \A w \in Workers : pc_worker[w] = "blocked" => woken[w]

(* Safety: workers only exit when stopping is true. *)
NoSpuriousExit ==
    \A w \in Workers : pc_worker[w] = "exited" => stopping

====
