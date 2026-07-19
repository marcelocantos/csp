---- MODULE ParkGate_Bug ----
(*******************************************************************************
 * Buggy variant of ParkGate: the notifier reads park_waiters_ BEFORE
 * publishing the state (equivalently: no seq_cst fence pairing, so the
 * count read can be hoisted above the publish).  TLC finds the lost
 * wakeup:
 *
 *   1. Notifier reads count = 0 → decides to skip the broadcast.
 *   2. Watcher registers, evaluates the predicate (state not yet
 *      published → false), blocks.
 *   3. Notifier publishes and finishes without notifying.
 *   4. Watcher sleeps forever on published state → NoLostWakeup fails.
 *******************************************************************************)

EXTENDS Integers

VARIABLES published, count, mu, woken, pc_w, pc_n

vars == <<published, count, mu, woken, pc_w, pc_n>>

Init ==
    /\ published = FALSE
    /\ count = 0
    /\ mu = "none"
    /\ woken = FALSE
    /\ pc_w = "start"
    /\ pc_n = "start"

WAcquire ==
    /\ pc_w = "start"
    /\ mu = "none"
    /\ mu' = "watcher"
    /\ pc_w' = "register"
    /\ UNCHANGED <<published, count, woken, pc_n>>

WRegister ==
    /\ pc_w = "register"
    /\ count' = count + 1
    /\ pc_w' = "eval"
    /\ UNCHANGED <<published, mu, woken, pc_n>>

WEval ==
    /\ pc_w = "eval"
    /\ mu = "watcher"
    /\ IF published
       THEN pc_w' = "deregister"
       ELSE pc_w' = "will_wait"
    /\ UNCHANGED <<published, count, mu, woken, pc_n>>

WEnterWait ==
    /\ pc_w = "will_wait"
    /\ mu = "watcher"
    /\ mu' = "none"
    /\ woken' = FALSE
    /\ pc_w' = "blocked"
    /\ UNCHANGED <<published, count, pc_n>>

WWoken ==
    /\ pc_w = "blocked"
    /\ woken
    /\ mu = "none"
    /\ mu' = "watcher"
    /\ pc_w' = "eval"
    /\ UNCHANGED <<published, count, woken, pc_n>>

WDone ==
    /\ pc_w = "deregister"
    /\ mu = "watcher"
    /\ count' = count - 1
    /\ mu' = "none"
    /\ pc_w' = "done"
    /\ UNCHANGED <<published, woken, pc_n>>

(* BUG: count is read first ... *)
NReadCount ==
    /\ pc_n = "start"
    /\ IF count = 0
       THEN pc_n' = "publish_skip"
       ELSE pc_n' = "publish_notify"
    /\ UNCHANGED <<published, count, mu, woken, pc_w>>

(* ... and the state is published afterwards. *)
NPublishSkip ==
    /\ pc_n = "publish_skip"
    /\ published' = TRUE
    /\ pc_n' = "done"
    /\ UNCHANGED <<count, mu, woken, pc_w>>

NPublishNotify ==
    /\ pc_n = "publish_notify"
    /\ published' = TRUE
    /\ pc_n' = "acquire_mu"
    /\ UNCHANGED <<count, mu, woken, pc_w>>

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

NNotify ==
    /\ pc_n = "notify"
    /\ woken' = IF pc_w = "blocked" THEN TRUE ELSE woken
    /\ pc_n' = "done"
    /\ UNCHANGED <<published, count, mu, pc_w>>

Next ==
    \/ WAcquire \/ WRegister \/ WEval \/ WEnterWait \/ WWoken \/ WDone
    \/ NReadCount \/ NPublishSkip \/ NPublishNotify
    \/ NAcquireMu \/ NReleaseMu \/ NNotify

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ published \in BOOLEAN
    /\ count \in 0..1
    /\ mu \in {"none", "watcher", "notifier"}
    /\ woken \in BOOLEAN
    /\ pc_w \in {"start", "register", "eval", "will_wait", "blocked",
                 "deregister", "done"}
    /\ pc_n \in {"start", "publish_skip", "publish_notify", "acquire_mu",
                 "release_mu", "notify", "done"}

NoLostWakeup ==
    (pc_n = "done" /\ pc_w = "blocked") => woken

====
