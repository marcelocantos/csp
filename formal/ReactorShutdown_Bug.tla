---- MODULE ReactorShutdown_Bug ----
(*******************************************************************************
 * BUGGY VERSION — demonstrates the race when shutdown skips the join
 * and clears writers while the reactor loop is still running.
 *
 * Bug: shutdown goes directly from setting stopping_ to clearing writers,
 * without joining the reactor thread first.  This allows fire_signal
 * (erasing from the map) to race with the cleanup (clearing the map).
 *
 * Expected result: TLC finds a NoRacyCleanup violation.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Timers

VARIABLES
    stopping,
    loop_alive,
    joined,
    timer_writers,
    pending_signals,
    reactor_pc,
    shutdown_pc,
    fire_batch

vars == <<stopping, loop_alive, joined, timer_writers, pending_signals,
          reactor_pc, shutdown_pc, fire_batch>>

Init ==
    /\ stopping = FALSE
    /\ loop_alive = TRUE
    /\ joined = FALSE
    /\ timer_writers = Timers
    /\ pending_signals = Cardinality(Timers)
    /\ reactor_pc = "check_stop"
    /\ shutdown_pc = "idle"
    /\ fire_batch = {}

(*******************************************************************************
 * REACTOR LOOP ACTIONS — identical to the correct version.
 ******************************************************************************)

ReactorCheckStop ==
    /\ reactor_pc = "check_stop"
    /\ loop_alive
    /\ IF stopping
       THEN /\ reactor_pc' = "loop_exited"
            /\ UNCHANGED <<stopping, loop_alive, joined, timer_writers,
                           pending_signals, shutdown_pc, fire_batch>>
       ELSE /\ \E batch \in SUBSET timer_writers :
                /\ fire_batch' = batch
                /\ reactor_pc' = IF batch = {} THEN "check_stop"
                                 ELSE "fire_one"
            /\ UNCHANGED <<stopping, loop_alive, joined, timer_writers,
                           pending_signals, shutdown_pc>>

ReactorFireOne ==
    /\ reactor_pc = "fire_one"
    /\ loop_alive
    /\ \E t \in fire_batch :
        /\ timer_writers' = timer_writers \ {t}
        /\ pending_signals' = pending_signals - 1
        /\ fire_batch' = fire_batch \ {t}
        /\ reactor_pc' = IF fire_batch \ {t} = {} THEN "check_stop"
                          ELSE "fire_one"
    /\ UNCHANGED <<stopping, loop_alive, joined, shutdown_pc>>

ReactorLoopExited ==
    /\ reactor_pc = "loop_exited"
    /\ loop_alive' = FALSE
    /\ reactor_pc' = "dead"
    /\ UNCHANGED <<stopping, joined, timer_writers, pending_signals,
                   shutdown_pc, fire_batch>>

(*******************************************************************************
 * SHUTDOWN ACTIONS — BUG: skip join, go straight to cleanup.
 ******************************************************************************)

(* Set stopping flag — same as correct version. *)
ShutdownSetStop ==
    /\ shutdown_pc = "idle"
    /\ stopping' = TRUE
    /\ shutdown_pc' = "cleanup"     \* BUG: skip join, go straight to cleanup
    /\ UNCHANGED <<loop_alive, joined, timer_writers, pending_signals,
                   reactor_pc, fire_batch>>

(* Clear remaining writers — BUG: reactor loop may still be running. *)
ShutdownCleanup ==
    /\ shutdown_pc = "cleanup"
    /\ timer_writers' = {}
    /\ pending_signals' = 0
    /\ shutdown_pc' = "done"
    /\ UNCHANGED <<stopping, loop_alive, joined, reactor_pc, fire_batch>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ ReactorCheckStop
    \/ ReactorFireOne
    \/ ReactorLoopExited
    \/ ShutdownSetStop
    \/ ShutdownCleanup

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ stopping \in BOOLEAN
    /\ loop_alive \in BOOLEAN
    /\ joined \in BOOLEAN
    /\ timer_writers \subseteq Timers
    /\ pending_signals \in -Cardinality(Timers)..Cardinality(Timers)
    /\ reactor_pc \in {"check_stop", "fire_one", "loop_exited", "dead"}
    /\ shutdown_pc \in {"idle", "cleanup", "done"}
    /\ fire_batch \subseteq Timers

(* Safety: cleanup only happens after the loop is dead. VIOLATED. *)
NoRacyCleanup ==
    shutdown_pc = "cleanup" => ~loop_alive /\ joined

NoFireAfterJoin ==
    joined => reactor_pc \in {"loop_exited", "dead"}

NonNegativePending ==
    pending_signals >= 0

====
