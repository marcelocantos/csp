---- MODULE AltStateCAS_Bug ----
(*******************************************************************************
 * BUGGY VERSION — replaces CAS with non-atomic check-then-set.
 *
 * In this version, the waker's claim of the waiter is split into two
 * separate steps: (1) check if alt_state == WAITING, (2) set alt_state
 * = CLAIMED. Another waker can interleave between these two steps,
 * causing double-claim.
 *
 * The dangerous interleaving:
 *   Waker1: reads alt_state == WAITING (check passes)
 *   Waker2: reads alt_state == WAITING (check passes — not yet set!)
 *   Waker1: sets alt_state = CLAIMED, sets signal = 1
 *   Waker2: sets alt_state = CLAIMED, sets signal = 2 (overwrites!)
 *   Result: both wakers think they won; signal is inconsistent;
 *           waiter may be scheduled twice.
 *
 * Expected result: TLC finds an ExactlyOneClaim violation.
 ******************************************************************************)

EXTENDS Integers

CONSTANTS
    Wakers

VARIABLES
    alt_state,
    signal,
    on_queue,
    pc_waiter,
    pc_waker

vars == <<alt_state, signal, on_queue, pc_waiter, pc_waker>>

Init ==
    /\ alt_state = "idle"
    /\ signal = "none"
    /\ on_queue = FALSE
    /\ pc_waiter = "will_register"
    /\ pc_waker = [w \in Wakers |-> "idle"]

(*******************************************************************************
 * WAITER ACTIONS — identical to correct version.
 ******************************************************************************)

WaiterRegister ==
    /\ pc_waiter = "will_register"
    /\ alt_state' = "waiting"
    /\ pc_waiter' = "suspended"
    /\ UNCHANGED <<signal, on_queue, pc_waker>>

WaiterDone ==
    /\ pc_waiter = "suspended"
    /\ on_queue = TRUE
    /\ alt_state' = "idle"
    /\ pc_waiter' = "done"
    /\ UNCHANGED <<signal, on_queue, pc_waker>>

(*******************************************************************************
 * WAKER ACTIONS — BUG: non-atomic check-then-set instead of CAS.
 *
 * The check (read alt_state) and the set (write alt_state) are separate
 * TLA+ actions, allowing interleaving between them. This models what
 * would happen if the code used:
 *
 *   if (alt_state.load() == ALT_WAITING) {   // step 1: check
 *       alt_state.store(ALT_CLAIMED);         // step 2: set
 *       ...
 *   }
 *
 * instead of:
 *   alt_state.compare_exchange_strong(expected=ALT_WAITING, ALT_CLAIMED)
 ******************************************************************************)

WakerStart(w) ==
    /\ pc_waker[w] = "idle"
    /\ alt_state = "waiting"
    /\ pc_waker' = [pc_waker EXCEPT ![w] = "check_state"]
    /\ UNCHANGED <<alt_state, signal, on_queue, pc_waiter>>

(* BUG step 1: non-atomic READ of alt_state. If WAITING, proceed to set.
 * Another waker can interleave here. *)
WakerCheck(w) ==
    /\ pc_waker[w] = "check_state"
    /\ IF alt_state \in {"waiting", "claimed"}
       THEN \* In buggy code, check only looks at original snapshot.
            \* We model this as: the check passed (saw WAITING at WakerStart),
            \* so unconditionally proceed to set. The interleaving window is
            \* between start and set.
            /\ pc_waker' = [pc_waker EXCEPT ![w] = "set_claimed"]
            /\ UNCHANGED <<alt_state, signal, on_queue, pc_waiter>>
       ELSE /\ pc_waker' = [pc_waker EXCEPT ![w] = "loser"]
            /\ UNCHANGED <<alt_state, signal, on_queue, pc_waiter>>

(* BUG step 2: non-atomic WRITE of alt_state = CLAIMED.
 * This always succeeds regardless of current state, because the
 * "check" already passed. *)
WakerSetClaimed(w) ==
    /\ pc_waker[w] = "set_claimed"
    /\ alt_state' = "claimed"
    /\ pc_waker' = [pc_waker EXCEPT ![w] = "set_signal"]
    /\ UNCHANGED <<signal, on_queue, pc_waiter>>

(* Winner sets signal — same as correct version. *)
WakerSetSignal(w) ==
    /\ pc_waker[w] = "set_signal"
    /\ signal' = w
    /\ pc_waker' = [pc_waker EXCEPT ![w] = "schedule"]
    /\ UNCHANGED <<alt_state, on_queue, pc_waiter>>

(* Winner calls schedule — same as correct version. *)
WakerSchedule(w) ==
    /\ pc_waker[w] = "schedule"
    /\ on_queue' = TRUE
    /\ pc_waker' = [pc_waker EXCEPT ![w] = "done_waker"]
    /\ UNCHANGED <<alt_state, signal, pc_waiter>>

LoserDone(w) ==
    /\ pc_waker[w] = "loser"
    /\ pc_waker' = [pc_waker EXCEPT ![w] = "done_waker"]
    /\ UNCHANGED <<alt_state, signal, on_queue, pc_waiter>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ WaiterRegister
    \/ WaiterDone
    \/ \E w \in Wakers :
        \/ WakerStart(w)
        \/ WakerCheck(w)
        \/ WakerSetClaimed(w)
        \/ WakerSetSignal(w)
        \/ WakerSchedule(w)
        \/ LoserDone(w)

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES — same invariants as correct version; should be VIOLATED.
 ******************************************************************************)

TypeOK ==
    /\ alt_state \in {"idle", "waiting", "claimed"}
    /\ signal \in ({"none"} \cup Wakers)
    /\ on_queue \in BOOLEAN
    /\ pc_waiter \in {"will_register", "suspended", "done"}
    /\ pc_waker \in [Wakers -> {"idle", "check_state", "set_claimed",
                                 "set_signal", "schedule", "loser", "done_waker"}]

ExactlyOneClaim ==
    \A w1 \in Wakers : \A w2 \in Wakers :
        (w1 /= w2) =>
            ~(pc_waker[w1] \in {"set_signal", "schedule"}
              /\ pc_waker[w2] \in {"set_signal", "schedule"})

SignalConsistency ==
    \A w \in Wakers :
        (signal = w) =>
            pc_waker[w] \in {"schedule", "done_waker"}

WaiterWakes ==
    (\A w \in Wakers : pc_waker[w] = "done_waker")
    => (on_queue \/ alt_state = "idle")

====
