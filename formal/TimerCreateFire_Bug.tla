---- MODULE TimerCreateFire_Bug ----
(*******************************************************************************
 * BUGGY VERSION — pending_signals is incremented AFTER registering
 * the OS timer.  The reactor can fire the timer before the increment,
 * causing pending_signals to go negative.
 *
 * Expected result: TLC finds a NonNegativePending violation.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Timers

VARIABLES
    pending_signals,
    timer_writers,
    registered,
    timer_pc,
    fired

vars == <<pending_signals, timer_writers, registered, timer_pc, fired>>

Init ==
    /\ pending_signals = 0
    /\ timer_writers = {}
    /\ registered = {}
    /\ timer_pc = [t \in Timers |-> "insert"]
    /\ fired = {}

(*******************************************************************************
 * CREATOR ACTIONS — BUG: register before increment.
 ******************************************************************************)

CreateInsert(t) ==
    /\ timer_pc[t] = "insert"
    /\ timer_writers' = timer_writers \cup {t}
    /\ timer_pc' = [timer_pc EXCEPT ![t] = "register"]  \* BUG: skip to register
    /\ UNCHANGED <<pending_signals, registered, fired>>

(* BUG: register OS timer BEFORE incrementing pending_signals. *)
CreateRegister(t) ==
    /\ timer_pc[t] = "register"
    /\ registered' = registered \cup {t}
    /\ timer_pc' = [timer_pc EXCEPT ![t] = "incr"]      \* BUG: increment after
    /\ UNCHANGED <<pending_signals, timer_writers, fired>>

CreateIncr(t) ==
    /\ timer_pc[t] = "incr"
    /\ pending_signals' = pending_signals + 1
    /\ timer_pc' = [timer_pc EXCEPT ![t] = "done"]
    /\ UNCHANGED <<timer_writers, registered, fired>>

(*******************************************************************************
 * REACTOR ACTIONS — identical to correct version.
 ******************************************************************************)

ReactorFire(t) ==
    /\ t \in registered
    /\ t \in timer_writers
    /\ t \notin fired
    /\ timer_writers' = timer_writers \ {t}
    /\ pending_signals' = pending_signals - 1
    /\ fired' = fired \cup {t}
    /\ UNCHANGED <<registered, timer_pc>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ \E t \in Timers :
        \/ CreateInsert(t)
        \/ CreateRegister(t)
        \/ CreateIncr(t)
        \/ ReactorFire(t)

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ pending_signals \in -Cardinality(Timers)..Cardinality(Timers)
    /\ timer_writers \subseteq Timers
    /\ registered \subseteq Timers
    /\ timer_pc \in [Timers -> {"insert", "register", "incr", "done"}]
    /\ fired \subseteq Timers

(* VIOLATED: fire before increment causes negative pending_signals. *)
NonNegativePending ==
    pending_signals >= 0

FireRequiresRegistration ==
    fired \subseteq registered

====
