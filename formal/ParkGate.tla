---- MODULE ParkGate ----
(*******************************************************************************
 * Models the park_waiters_ gate on park_cv notifications (🎯T34 O3).
 *
 * unpark_one / worker parks / imp completion used to broadcast park_cv
 * unconditionally on every scheduler event — a pthread syscall per
 * channel rendezvous even when nothing waits on park_cv (only
 * completion/quiescence watchers do: main_loop, run(), await_idle,
 * await_quiescent, quiescent_loop).  The gate skips the broadcast when
 * no watcher is registered.
 *
 * Protocol (src/runtime.cpp Runtime::park_wait / notify_watchers):
 *   Watcher:  lock park_mu; park_waiters_++; [seq_cst fence]
 *             cv.wait(lk, pred); park_waiters_--
 *   Notifier: publish state (e.g. has_global_work_=true);
 *             [seq_cst fence]; if (park_waiters_ == 0) skip;
 *             else { lock park_mu; } cv.notify_all()
 *
 * The seq_cst fences justify modeling the actions with TLA+'s
 * sequentially consistent interleaving semantics: they order the
 * watcher's (register ; read state) against the notifier's
 * (publish ; read count) so that at least one side observes the other
 * (the eventcount argument).  Under SC there are two cases:
 *   - Notifier reads count after the watcher registered → sees 1 →
 *     takes the empty critical section (serializing with the
 *     pred-evaluated-but-not-yet-blocked gap) and notifies.
 *   - Notifier reads count before the watcher registered → the
 *     watcher's later predicate evaluation sees the published state
 *     and never blocks.
 *
 * ParkGate_Bug.tla reorders the notifier (read count before publish)
 * and TLC finds the lost wakeup.
 *******************************************************************************)

EXTENDS Integers

VARIABLES
    published,   \* Boolean: the state the watcher waits for (work/completion)
    count,       \* 0..1: park_waiters_ (one watcher modeled)
    mu,          \* "none" | "watcher" | "notifier" — who holds park_mu
    woken,       \* Boolean: CV wake signal delivered to the blocked watcher
    pc_w,        \* watcher program counter
    pc_n         \* notifier program counter

vars == <<published, count, mu, woken, pc_w, pc_n>>

Init ==
    /\ published = FALSE
    /\ count = 0
    /\ mu = "none"
    /\ woken = FALSE
    /\ pc_w = "start"
    /\ pc_n = "start"

(*******************************************************************************
 * WATCHER — Runtime::park_wait(pred)
 *******************************************************************************)

(* lock park_mu *)
WAcquire ==
    /\ pc_w = "start"
    /\ mu = "none"
    /\ mu' = "watcher"
    /\ pc_w' = "register"
    /\ UNCHANGED <<published, count, woken, pc_n>>

(* park_waiters_++ (under mu; fence follows) *)
WRegister ==
    /\ pc_w = "register"
    /\ count' = count + 1
    /\ pc_w' = "eval"
    /\ UNCHANGED <<published, mu, woken, pc_n>>

(* Evaluate cv.wait predicate with the lock held.  TRUE → done (no
 * block).  FALSE → will enter cv.wait; the gap between this step and
 * WEnterWait is where mu is held but the watcher is not yet blocked. *)
WEval ==
    /\ pc_w = "eval"
    /\ mu = "watcher"
    /\ IF published
       THEN pc_w' = "deregister"
       ELSE pc_w' = "will_wait"
    /\ UNCHANGED <<published, count, mu, woken, pc_n>>

(* cv.wait(lk): atomically release the lock and block. *)
WEnterWait ==
    /\ pc_w = "will_wait"
    /\ mu = "watcher"
    /\ mu' = "none"
    /\ woken' = FALSE
    /\ pc_w' = "blocked"
    /\ UNCHANGED <<published, count, pc_n>>

(* Wake: reacquire the lock, recheck the predicate. *)
WWoken ==
    /\ pc_w = "blocked"
    /\ woken
    /\ mu = "none"
    /\ mu' = "watcher"
    /\ pc_w' = "eval"
    /\ UNCHANGED <<published, count, woken, pc_n>>

(* park_waiters_--; unlock; return. *)
WDone ==
    /\ pc_w = "deregister"
    /\ mu = "watcher"
    /\ count' = count - 1
    /\ mu' = "none"
    /\ pc_w' = "done"
    /\ UNCHANGED <<published, woken, pc_n>>

(*******************************************************************************
 * NOTIFIER — publish state, then Runtime::notify_watchers()
 *******************************************************************************)

(* Publish the state change (release store in the C++; the seq_cst
 * fence after it is what licenses the SC interleaving model). *)
NPublish ==
    /\ pc_n = "start"
    /\ published' = TRUE
    /\ pc_n' = "read_count"
    /\ UNCHANGED <<count, mu, woken, pc_w>>

(* Read park_waiters_.  Zero → skip the broadcast entirely. *)
NReadCount ==
    /\ pc_n = "read_count"
    /\ IF count = 0
       THEN pc_n' = "done"
       ELSE pc_n' = "acquire_mu"
    /\ UNCHANGED <<published, count, mu, woken, pc_w>>

(* Empty critical section: forces any watcher that evaluated the
 * predicate false (lock held, not yet blocked) to reach cv.wait
 * before the notify fires. *)
NAcquireMu ==
    /\ pc_n = "acquire_mu"
    /\ mu = "none"
    /\ mu' = "notifier"
    /\ pc_n' = "release_mu"
    /\ UNCHANGED <<published, count, woken, pc_w>>

NReleaseMu ==
    /\ pc_n = "release_mu"
    /\ mu = "notifier"
    /\ mu' = "none"
    /\ pc_n' = "notify"
    /\ UNCHANGED <<published, count, woken, pc_w>>

(* notify_all: only a blocked watcher receives the signal. *)
NNotify ==
    /\ pc_n = "notify"
    /\ woken' = IF pc_w = "blocked" THEN TRUE ELSE woken
    /\ pc_n' = "done"
    /\ UNCHANGED <<published, count, mu, pc_w>>

Next ==
    \/ WAcquire \/ WRegister \/ WEval \/ WEnterWait \/ WWoken \/ WDone
    \/ NPublish \/ NReadCount \/ NAcquireMu \/ NReleaseMu \/ NNotify

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 *******************************************************************************)

TypeOK ==
    /\ published \in BOOLEAN
    /\ count \in 0..1
    /\ mu \in {"none", "watcher", "notifier"}
    /\ woken \in BOOLEAN
    /\ pc_w \in {"start", "register", "eval", "will_wait", "blocked",
                 "deregister", "done"}
    /\ pc_n \in {"start", "read_count", "acquire_mu", "release_mu",
                 "notify", "done"}

(* Safety: once the notifier has finished (state published, broadcast
 * either delivered or skipped), a blocked watcher must hold a wake
 * signal — otherwise it sleeps forever on published state. *)
NoLostWakeup ==
    (pc_n = "done" /\ pc_w = "blocked") => woken

====
