---- MODULE PerWorkerWake_Bug ----
(*******************************************************************************
 * BUGGY VERSION — demonstrates the thundering-herd / lost-wakeup scenario
 * with the original shared notify_all protocol (pre-T13).
 *
 * In this version unpark_one() calls notify_all on a shared condvar, waking
 * ALL sleeping workers. The NoteConsistency invariant is trivially satisfied
 * but ExactlyOneWoken is violated: multiple workers can be woken per call.
 *
 * We model the bug as: UnparkNotifyAll wakes every sleeping worker at once.
 * Expected result: TLC finds a AllSleepersWoken violation when 2 workers sleep.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Workers

NOTE_AWAKE   == 1
NOTE_SLEEPING == 0

VARIABLES
    stopping,
    note,
    parked,
    pc_worker,
    pc_shutdown,
    pc_sched,
    woken_count   \* How many workers were woken by the last unpark_one call

vars == <<stopping, note, parked, pc_worker, pc_shutdown, pc_sched, woken_count>>

Init ==
    /\ stopping = FALSE
    /\ note = [w \in Workers |-> NOTE_AWAKE]
    /\ parked = [w \in Workers |-> FALSE]
    /\ pc_worker = [w \in Workers |-> "check_work"]
    /\ pc_shutdown = "idle"
    /\ pc_sched = "idle"
    /\ woken_count = 0

WorkerCheckWork(w) ==
    /\ pc_worker[w] = "check_work"
    /\ IF stopping
       THEN /\ pc_worker' = [pc_worker EXCEPT ![w] = "exited"]
            /\ UNCHANGED <<stopping, note, parked, pc_shutdown, pc_sched, woken_count>>
       ELSE /\ pc_worker' = [pc_worker EXCEPT ![w] = "set_parked"]
            /\ UNCHANGED <<stopping, note, parked, pc_shutdown, pc_sched, woken_count>>

WorkerSetParked(w) ==
    /\ pc_worker[w] = "set_parked"
    /\ parked' = [parked EXCEPT ![w] = TRUE]
    /\ note' = [note EXCEPT ![w] = NOTE_SLEEPING]
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "sleeping"]
    /\ UNCHANGED <<stopping, pc_shutdown, pc_sched, woken_count>>

WorkerWoken(w) ==
    /\ pc_worker[w] = "sleeping"
    /\ note[w] = NOTE_AWAKE
    /\ parked' = [parked EXCEPT ![w] = FALSE]
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "check_work"]
    /\ UNCHANGED <<stopping, note, pc_shutdown, pc_sched, woken_count>>

(* BUG: notify_all wakes ALL sleeping workers, not just one. *)
SchedUnparkAll ==
    /\ pc_sched = "idle"
    /\ \E w \in Workers : note[w] = NOTE_SLEEPING
    /\ LET sleepers == {w \in Workers : note[w] = NOTE_SLEEPING}
       IN /\ note' = [w \in Workers |-> IF w \in sleepers THEN NOTE_AWAKE ELSE note[w]]
          /\ woken_count' = Cardinality(sleepers)
          /\ pc_sched' = "done"
    /\ UNCHANGED <<stopping, parked, pc_worker, pc_shutdown>>

SchedDone ==
    /\ pc_sched = "done"
    /\ pc_sched' = "idle"
    /\ woken_count' = 0
    /\ UNCHANGED <<stopping, note, parked, pc_worker, pc_shutdown>>

ShutdownSetFlag ==
    /\ pc_shutdown = "idle"
    /\ stopping' = TRUE
    /\ note' = [w \in Workers |-> NOTE_AWAKE]
    /\ pc_shutdown' = "done"
    /\ UNCHANGED <<parked, pc_worker, pc_sched, woken_count>>

Next ==
    \/ \E w \in Workers :
        \/ WorkerCheckWork(w)
        \/ WorkerSetParked(w)
        \/ WorkerWoken(w)
    \/ SchedUnparkAll
    \/ SchedDone
    \/ ShutdownSetFlag

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ stopping \in BOOLEAN
    /\ note \in [Workers -> {NOTE_SLEEPING, NOTE_AWAKE}]
    /\ parked \in [Workers -> BOOLEAN]
    /\ pc_worker \in [Workers -> {"check_work", "set_parked", "sleeping", "exited"}]
    /\ pc_shutdown \in {"idle", "done"}
    /\ pc_sched \in {"idle", "done"}
    /\ woken_count \in 0..Cardinality(Workers)

(* The bug: unpark_one can wake more than 1 worker in a single call. *)
AtMostOneWoken ==
    woken_count <= 1

NoSpuriousExit ==
    \A w \in Workers : pc_worker[w] = "exited" => stopping

====
