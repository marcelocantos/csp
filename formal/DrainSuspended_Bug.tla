---- MODULE DrainSuspended_Bug ----
(*******************************************************************************
 * BUGGY VERSION — demonstrates the TOCTOU race that global_mu prevents.
 *
 * In this version, drain_suspended does NOT hold global_mu. The clear-
 * suspending and check-wake_pending steps are separate non-atomic actions,
 * allowing schedule() to interleave between them.
 *
 * The dangerous interleaving:
 *   drain: read wake_pending=false
 *   schedule: suspending=true → set wake_pending=true
 *   drain: clear suspending
 *   Result: suspending=false, wake_pending=true, on_queue=false — LOST!
 *
 * Expected result: TLC finds a NoLostWakeup violation.
 *******************************************************************************)

EXTENDS Integers

VARIABLES
    suspending,
    wake_pending,
    on_queue,
    global_mu,      \* Only used by schedule() in this buggy version
    pc_mt,
    pc_waker

vars == <<suspending, wake_pending, on_queue, global_mu, pc_mt, pc_waker>>

Init ==
    /\ suspending = FALSE
    /\ wake_pending = FALSE
    /\ on_queue = TRUE
    /\ global_mu = "none"
    /\ pc_mt = "running"
    /\ pc_waker = "idle"

(*******************************************************************************
 * MT ACTIONS — same as correct version except drain is split.
 *******************************************************************************)

BeginSuspend ==
    /\ pc_mt = "running"
    /\ suspending' = TRUE
    /\ on_queue' = FALSE
    /\ pc_mt' = "check_wp"
    /\ UNCHANGED <<wake_pending, global_mu, pc_waker>>

CheckWP ==
    /\ pc_mt = "check_wp"
    /\ IF wake_pending
       THEN /\ wake_pending' = FALSE
            /\ on_queue' = TRUE
            /\ pc_mt' = "clear_susp"
       ELSE /\ pc_mt' = "switched_out"
            /\ UNCHANGED <<wake_pending, on_queue>>
    /\ UNCHANGED <<suspending, global_mu, pc_waker>>

ClearSusp ==
    /\ pc_mt = "clear_susp"
    /\ suspending' = FALSE
    /\ pc_mt' = "done"
    /\ UNCHANGED <<wake_pending, on_queue, global_mu, pc_waker>>

(*******************************************************************************
 * BUG: drain without mutex — two separate non-atomic steps.
 *
 * The buggy split checks wake_pending first, THEN clears suspending.
 * schedule() can interleave between them: it sees suspending=true and
 * sets wake_pending=true, but drain already checked wake_pending (saw
 * false) and never looks again.
 *******************************************************************************)

(* Buggy drain step 1: check wake_pending WITHOUT holding lock *)
DrainCheckWP ==
    /\ pc_mt = "switched_out"
    /\ IF wake_pending
       THEN /\ wake_pending' = FALSE
            /\ on_queue' = TRUE
       ELSE UNCHANGED <<wake_pending, on_queue>>
    /\ pc_mt' = "drain_clear"
    /\ UNCHANGED <<suspending, global_mu, pc_waker>>

(* Buggy drain step 2: clear suspending WITHOUT holding lock *)
DrainClearSusp ==
    /\ pc_mt = "drain_clear"
    /\ suspending' = FALSE
    /\ pc_mt' = "done"
    /\ UNCHANGED <<wake_pending, on_queue, global_mu, pc_waker>>

(*******************************************************************************
 * WAKER ACTIONS — identical to correct version (schedule still uses
 * global_mu; the bug is that drain doesn't).
 *******************************************************************************)

StartWake ==
    /\ pc_waker = "idle"
    /\ pc_mt /= "running"
    /\ pc_waker' = "want_lock"
    /\ UNCHANGED <<suspending, wake_pending, on_queue, global_mu, pc_mt>>

AcquireWake ==
    /\ pc_waker = "want_lock"
    /\ global_mu = "none"
    /\ global_mu' = "sched"
    /\ pc_waker' = "scheduling"
    /\ UNCHANGED <<suspending, wake_pending, on_queue, pc_mt>>

DoSchedule ==
    /\ pc_waker = "scheduling"
    /\ IF suspending
       THEN /\ wake_pending' = TRUE
            /\ UNCHANGED on_queue
       ELSE /\ on_queue' = TRUE
            /\ UNCHANGED wake_pending
    /\ global_mu' = "none"
    /\ pc_waker' = "done_waker"
    /\ UNCHANGED <<suspending, pc_mt>>

(*******************************************************************************
 * SPECIFICATION
 *******************************************************************************)

Next ==
    \/ BeginSuspend
    \/ CheckWP
    \/ ClearSusp
    \/ DrainCheckWP      \* Replaces AcquireDrain + Drain
    \/ DrainClearSusp
    \/ StartWake
    \/ AcquireWake
    \/ DoSchedule

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 *******************************************************************************)

TypeOK ==
    /\ suspending \in BOOLEAN
    /\ wake_pending \in BOOLEAN
    /\ on_queue \in BOOLEAN
    /\ global_mu \in {"none", "drain", "sched"}
    /\ pc_mt \in {"running", "check_wp", "clear_susp",
                   "switched_out", "drain_clear", "done"}
    /\ pc_waker \in {"idle", "want_lock", "scheduling", "done_waker"}

NoLostWakeup ==
    (pc_mt = "done" /\ pc_waker = "done_waker") => on_queue

====
