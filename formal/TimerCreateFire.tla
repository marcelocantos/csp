---- MODULE TimerCreateFire ----
(*******************************************************************************
 * Models the ordering between create_timer (incrementing pending_signals)
 * and fire_signal (decrementing pending_signals) to verify that
 * pending_signals never goes negative and the scheduler exit check
 * is safe.
 *
 * create_timer must increment pending_signals BEFORE registering the
 * OS timer (kevent/timerfd).  If the increment happens after registration,
 * the timer can fire before the increment, causing pending_signals to
 * go negative — the scheduler would see has_pending_signals()==false
 * prematurely.
 *
 * Participants:
 *   Creator  — thread calling create_timer (inserts writer, increments
 *              pending_signals, registers OS timer)
 *   Reactor  — loop thread that fires the timer (erases writer,
 *              decrements pending_signals)
 *   Scheduler — checks pending_signals for exit decision
 *
 * Code references:
 *   CreateInsert   — reactor.cc:87-89 (emplace writer under signal_mu)
 *   CreateIncr     — reactor.cc:90 (pending_signals++)
 *   CreateRegister — reactor.cc:92-96 (kevent EV_ADD)
 *   ReactorFire    — reactor.cc:148-164 (erase writer, pending_signals--)
 *   SchedCheck     — csp.cc:28 (!has_pending_signals())
 *
 * Expected result: TLC finds no violations.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Timers      \* set of timer IDs, e.g. {t1, t2}

VARIABLES
    pending_signals,    \* Int: pending signal count
    timer_writers,      \* set of timer IDs with live writers in the map
    registered,         \* set of timer IDs registered in the OS
    timer_pc,           \* [Timers -> creator program counter]
    fired               \* set of timer IDs that have been fired

vars == <<pending_signals, timer_writers, registered, timer_pc, fired>>

Init ==
    /\ pending_signals = 0
    /\ timer_writers = {}
    /\ registered = {}
    /\ timer_pc = [t \in Timers |-> "insert"]
    /\ fired = {}

(*******************************************************************************
 * CREATOR ACTIONS (one per timer, sequential within each timer)
 ******************************************************************************)

(* Insert writer into map under signal_mu.
 * Code: reactor.cc:87-89
 *
 * TLA:TimerCreateFire.CreateInsert *)
CreateInsert(t) ==
    /\ timer_pc[t] = "insert"
    /\ timer_writers' = timer_writers \cup {t}
    /\ timer_pc' = [timer_pc EXCEPT ![t] = "incr"]
    /\ UNCHANGED <<pending_signals, registered, fired>>

(* Increment pending_signals.
 * Code: reactor.cc:90
 *
 * TLA:TimerCreateFire.CreateIncr *)
CreateIncr(t) ==
    /\ timer_pc[t] = "incr"
    /\ pending_signals' = pending_signals + 1
    /\ timer_pc' = [timer_pc EXCEPT ![t] = "register"]
    /\ UNCHANGED <<timer_writers, registered, fired>>

(* Register OS timer — after this, the reactor can fire it.
 * Code: reactor.cc:92-96
 *
 * TLA:TimerCreateFire.CreateRegister *)
CreateRegister(t) ==
    /\ timer_pc[t] = "register"
    /\ registered' = registered \cup {t}
    /\ timer_pc' = [timer_pc EXCEPT ![t] = "done"]
    /\ UNCHANGED <<pending_signals, timer_writers, fired>>

(*******************************************************************************
 * REACTOR ACTIONS
 ******************************************************************************)

(* Reactor fires a registered timer: erase writer, decrement pending.
 * Can only fire timers that are registered AND have a writer.
 *
 * Code: reactor.cc:148-164
 *
 * TLA:TimerCreateFire.ReactorFire *)
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
        \/ CreateIncr(t)
        \/ CreateRegister(t)
        \/ ReactorFire(t)

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ pending_signals \in 0..Cardinality(Timers)
    /\ timer_writers \subseteq Timers
    /\ registered \subseteq Timers
    /\ timer_pc \in [Timers -> {"insert", "incr", "register", "done"}]
    /\ fired \subseteq Timers

(* Safety: pending_signals never goes negative. *)
NonNegativePending ==
    pending_signals >= 0

(* Safety: a timer can only fire after it has been registered. *)
FireRequiresRegistration ==
    fired \subseteq registered

(* Safety: pending_signals accurately counts unfired timers that have
 * been incremented.  Specifically: pending = (incremented - fired). *)
PendingAccurate ==
    pending_signals = Cardinality({t \in Timers : timer_pc[t] \in {"register", "done"}} \ fired)

====
