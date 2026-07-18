---- MODULE DrainSuspended_Bug ----
(*******************************************************************************
 * Buggy variant of DrainSuspended: drain_suspended reads the state and
 * clears it in TWO steps (load, then store IDLE) instead of one atomic
 * exchange.  A waker's CAS(SUSP → WAKE) can land between them; the
 * store then wipes the WAKE record and nobody queues the MT:
 *
 *   1. Drain loads state = SUSP (no wake yet).
 *   2. Waker CASes SUSP → WAKE, returns (wake "recorded").
 *   3. Drain stores IDLE — the WAKE is erased, no push happens.
 *   4. MT sleeps forever → NoLostWakeup fails.
 *
 * This is the same TOCTOU shape the old two-flag protocol (separate
 * suspending_/wake_pending_ booleans) had to close with the global
 * mutex; the single-word protocol closes it with the atomic exchange
 * instead.
 *******************************************************************************)

EXTENDS Integers

VARIABLES state, on_queue, waker_pushed, pc_mt, pc_waker

vars == <<state, on_queue, waker_pushed, pc_mt, pc_waker>>

Init ==
    /\ state = "idle"
    /\ on_queue = TRUE
    /\ waker_pushed = FALSE
    /\ pc_mt = "running"
    /\ pc_waker = "idle"

BeginSuspend ==
    /\ pc_mt = "running"
    /\ state' = "susp"
    /\ on_queue' = FALSE
    /\ pc_mt' = "check_wp"
    /\ UNCHANGED <<waker_pushed, pc_waker>>

CheckWP ==
    /\ pc_mt = "check_wp"
    /\ IF state = "wake"
       THEN /\ state' = "idle"
            /\ on_queue' = TRUE
            /\ pc_mt' = "done"
       ELSE /\ pc_mt' = "switched_out"
            /\ UNCHANGED <<state, on_queue>>
    /\ UNCHANGED <<waker_pushed, pc_waker>>

(* BUG: the drain is split — load first ... *)
DrainLoad ==
    /\ pc_mt = "switched_out"
    /\ IF state = "wake"
       THEN pc_mt' = "drain_push"
       ELSE pc_mt' = "drain_clear"
    /\ UNCHANGED <<state, on_queue, waker_pushed, pc_waker>>

(* ... then the clear (and push, when the load saw WAKE) happen in
 * separate steps, so a waker's CAS can slip in between. *)
DrainPush ==
    /\ pc_mt = "drain_push"
    /\ state' = "idle"
    /\ on_queue' = TRUE
    /\ pc_mt' = "done"
    /\ UNCHANGED <<waker_pushed, pc_waker>>

DrainClear ==
    /\ pc_mt = "drain_clear"
    /\ state' = "idle"
    /\ pc_mt' = "done"
    /\ UNCHANGED <<on_queue, waker_pushed, pc_waker>>

StartWake ==
    /\ pc_waker = "idle"
    /\ pc_mt /= "running"
    /\ pc_waker' = "waking"
    /\ UNCHANGED <<state, on_queue, waker_pushed, pc_mt>>

WakerStep ==
    /\ pc_waker = "waking"
    /\ IF state = "susp"
       THEN /\ state' = "wake"
            /\ pc_waker' = "done_waker"
            /\ UNCHANGED <<on_queue, waker_pushed, pc_mt>>
       ELSE IF state = "wake"
       THEN /\ pc_waker' = "done_waker"
            /\ UNCHANGED <<state, on_queue, waker_pushed, pc_mt>>
       ELSE /\ on_queue' = TRUE
            /\ waker_pushed' = TRUE
            /\ pc_waker' = "done_waker"
            /\ UNCHANGED <<state, pc_mt>>

Next ==
    \/ BeginSuspend
    \/ CheckWP
    \/ DrainLoad
    \/ DrainPush
    \/ DrainClear
    \/ StartWake
    \/ WakerStep

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ state \in {"idle", "susp", "wake"}
    /\ on_queue \in BOOLEAN
    /\ waker_pushed \in BOOLEAN
    /\ pc_mt \in {"running", "check_wp", "switched_out", "drain_push",
                  "drain_clear", "done"}
    /\ pc_waker \in {"idle", "waking", "done_waker"}

NoLostWakeup ==
    (pc_mt = "done" /\ pc_waker = "done_waker") => on_queue

NoPrematurePush ==
    waker_pushed => pc_mt = "done"

====
