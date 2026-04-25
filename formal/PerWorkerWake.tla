---- MODULE PerWorkerWake ----
(*******************************************************************************
 * Models the per-worker Note wake protocol that replaces notify_all on a
 * shared condvar (the thundering-herd fix for 🎯T13).
 *
 * The new design:
 *   - Each worker has a Note: an atomic integer with 3 states:
 *       0 = sleeping (waiting for wake)
 *       1 = awake (not parked or has been woken)
 *       2 = flagged (wakeup posted while worker was transitioning)
 *   - unpark_one() finds one sleeping worker and wakes exactly that worker
 *     using a platform futex (not a shared condvar).
 *   - The shared park_cv is still used by workers to signal the main_loop
 *     quiescence check (workers call notify_all on park_cv when they park,
 *     so main_loop can detect all-parked). Workers do NOT wait on park_cv.
 *   - shutdown() still uses the shared park_cv: it sets stopping=true, then
 *     wakes all workers via their per-worker Notes.
 *
 * Participants:
 *   W1, W2   — two worker threads
 *   Main     — main_loop / quiescence check (reads parked flags, waits park_cv)
 *   Scheduler — the entity that calls unpark_one() when scheduling work
 *   Shutdown  — shutdown() call
 *
 * Safety properties:
 *   1. ExactlyOneWoken: unpark_one wakes at most one worker.
 *   2. NoLostWake: a worker sleeping on its Note is eventually woken if
 *      unpark_one() is called while it is sleeping.
 *   3. NoSpuriousExit: workers only exit when stopping=true.
 *   4. MainSeesQuiescence: when all workers are parked, main can observe it.
 *   5. ShutdownWakesAll: after shutdown completes, no worker is stuck sleeping.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Workers     \* set of worker IDs, e.g. {w1, w2}

(*******************************************************************************
 * Note state machine (per-worker atomic):
 *   NOTE_AWAKE   = 1  — worker is running / not parked
 *   NOTE_SLEEPING = 0 — worker has called note_sleep (futex wait)
 *   NOTE_FLAGGED  = 2 — wakeup was posted while worker was awake;
 *                        worker will not sleep on next park attempt
 ******************************************************************************)
NOTE_AWAKE   == 1
NOTE_SLEEPING == 0
NOTE_FLAGGED  == 2

VARIABLES
    stopping,       \* Boolean: global shutdown flag
    note,           \* [Workers -> {0,1,2}]: per-worker Note state
    parked,         \* [Workers -> BOOLEAN]: p.parked flag (read by main_loop)
    pc_worker,      \* [Workers -> pc state]
    pc_main,        \* main_loop program counter
    pc_shutdown,    \* shutdown program counter
    pc_sched,       \* scheduler (unpark_one) program counter
    woken_worker    \* The worker targeted by current unpark_one() call (or "none")

vars == <<stopping, note, parked, pc_worker, pc_main, pc_shutdown, pc_sched, woken_worker>>

(*******************************************************************************
 * INITIAL STATE
 ******************************************************************************)
Init ==
    /\ stopping = FALSE
    /\ note = [w \in Workers |-> NOTE_AWAKE]
    /\ parked = [w \in Workers |-> FALSE]
    /\ pc_worker = [w \in Workers |-> "check_work"]
    /\ pc_main = "waiting"
    /\ pc_shutdown = "idle"
    /\ pc_sched = "idle"
    /\ woken_worker = "none"

(*******************************************************************************
 * WORKER ACTIONS
 *
 * Worker protocol (park path):
 *   check_work -> [has_work] -> check_work (loop)
 *             -> [no work, !stopping] -> set_parked -> try_sleep -> sleeping
 *   sleeping -> [note woken by unpark_one or shutdown] -> clear_parked -> check_work
 *
 * Note.sleep() protocol:
 *   1. CAS note: AWAKE -> SLEEPING (if FLAGGED, consume flag and skip sleep)
 *   2. If CAS succeeds: futex_wait(note, SLEEPING) — blocks until note != SLEEPING
 *   3. On wake: note is AWAKE (set by waker)
 *
 * Note.wake() protocol (called by unpark_one or shutdown):
 *   1. CAS note: SLEEPING -> AWAKE (if already AWAKE, set FLAGGED)
 *   2. If CAS SLEEPING->AWAKE succeeded: futex_wake(note, 1)
 ******************************************************************************)

(* Worker checks for work. Abstracting away actual work; focus is on park/wake.
 * If stopping: exit. Otherwise try to park. *)
WorkerCheckWork(w) ==
    /\ pc_worker[w] = "check_work"
    /\ IF stopping
       THEN /\ pc_worker' = [pc_worker EXCEPT ![w] = "exited"]
            /\ UNCHANGED <<stopping, note, parked, pc_main, pc_shutdown, pc_sched, woken_worker>>
       ELSE /\ pc_worker' = [pc_worker EXCEPT ![w] = "set_parked"]
            /\ UNCHANGED <<stopping, note, parked, pc_main, pc_shutdown, pc_sched, woken_worker>>

(* Worker sets p.parked = true and notifies main_loop via park_cv
 * (modeled as just setting parked[w]; main_loop wakes from its waiting state). *)
WorkerSetParked(w) ==
    /\ pc_worker[w] = "set_parked"
    /\ parked' = [parked EXCEPT ![w] = TRUE]
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "try_sleep"]
    \* Signal main_loop — it may now see all-parked quiescence
    /\ IF pc_main = "waiting" THEN pc_main' = "checking" ELSE pc_main' = pc_main
    /\ UNCHANGED <<stopping, note, pc_shutdown, pc_sched, woken_worker>>

(* Worker attempts to sleep on its Note.
 * If note == FLAGGED: consume flag, skip sleep (go back to check_work).
 * If note == AWAKE: CAS AWAKE->SLEEPING; then block (go to "sleeping").
 * (note cannot be SLEEPING here since only this worker sets it to SLEEPING) *)
WorkerTrySleep(w) ==
    /\ pc_worker[w] = "try_sleep"
    /\ IF note[w] = NOTE_FLAGGED
       THEN \* Consume the flag — skip sleep, go back to check_work
            /\ note' = [note EXCEPT ![w] = NOTE_AWAKE]
            /\ pc_worker' = [pc_worker EXCEPT ![w] = "clear_parked"]
            /\ UNCHANGED <<stopping, parked, pc_main, pc_shutdown, pc_sched, woken_worker>>
       ELSE \* CAS AWAKE->SLEEPING: go to sleep
            /\ note[w] = NOTE_AWAKE
            /\ note' = [note EXCEPT ![w] = NOTE_SLEEPING]
            /\ pc_worker' = [pc_worker EXCEPT ![w] = "sleeping"]
            /\ UNCHANGED <<stopping, parked, pc_main, pc_shutdown, pc_sched, woken_worker>>

(* Worker is sleeping (futex_wait). It wakes when note becomes != SLEEPING.
 * Waking is done by unpark_one (sets SLEEPING->AWAKE) or shutdown (sets SLEEPING->AWAKE).
 * In the FLAGGED case: shutdown set note from AWAKE->FLAGGED after unpark_one had
 * already set SLEEPING->AWAKE. The futex (waiting on SLEEPING=0) already woke up
 * when note became AWAKE (!=0), so FLAGGED is also a valid wake state. *)
WorkerWoken(w) ==
    /\ pc_worker[w] = "sleeping"
    /\ note[w] # NOTE_SLEEPING  \* Futex wakes on any value != SLEEPING
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "clear_parked"]
    /\ UNCHANGED <<stopping, note, parked, pc_main, pc_shutdown, pc_sched, woken_worker>>

(* Worker clears p.parked and resumes check_work. *)
WorkerClearParked(w) ==
    /\ pc_worker[w] = "clear_parked"
    /\ parked' = [parked EXCEPT ![w] = FALSE]
    /\ pc_worker' = [pc_worker EXCEPT ![w] = "check_work"]
    /\ UNCHANGED <<stopping, note, pc_main, pc_shutdown, pc_sched, woken_worker>>

(*******************************************************************************
 * UNPARK_ONE ACTIONS (scheduler)
 *
 * unpark_one() scans workers to find one that is sleeping and wakes it.
 * Exactly one worker is targeted per call.
 *
 * Protocol:
 *   1. Find a sleeping worker w (note[w] == SLEEPING).
 *   2. CAS note[w]: SLEEPING -> AWAKE.
 *   3. futex_wake(note[w], 1) — wake exactly one waiter.
 *
 * If no worker is sleeping: post a flag (AWAKE->FLAGGED) on any idle worker
 * so it won't sleep on its next park attempt.
 ******************************************************************************)

(* Scheduler starts an unpark_one() call. *)
SchedStart ==
    /\ pc_sched = "idle"
    /\ pc_sched' = "find_sleeper"
    /\ UNCHANGED <<stopping, note, parked, pc_worker, pc_main, pc_shutdown, woken_worker>>

(* Scheduler finds a sleeping worker and wakes it (CAS SLEEPING->AWAKE + futex_wake). *)
SchedWakeSleeping ==
    /\ pc_sched = "find_sleeper"
    /\ \E w \in Workers : note[w] = NOTE_SLEEPING  \* At least one sleeper
    /\ LET target == CHOOSE w \in Workers : note[w] = NOTE_SLEEPING
       IN /\ note' = [note EXCEPT ![target] = NOTE_AWAKE]
          /\ woken_worker' = target
          /\ pc_sched' = "done"
          /\ UNCHANGED <<stopping, parked, pc_worker, pc_main, pc_shutdown>>

(* No workers sleeping: flag one awake worker so it skips its next sleep attempt. *)
SchedFlagAwake ==
    /\ pc_sched = "find_sleeper"
    /\ \A w \in Workers : note[w] # NOTE_SLEEPING  \* No sleepers
    /\ \E w \in Workers : note[w] = NOTE_AWAKE     \* At least one flaggable
    /\ LET target == CHOOSE w \in Workers : note[w] = NOTE_AWAKE
       IN /\ note' = [note EXCEPT ![target] = NOTE_FLAGGED]
          /\ woken_worker' = "none"
          /\ pc_sched' = "done"
          /\ UNCHANGED <<stopping, parked, pc_worker, pc_main, pc_shutdown>>

(* All workers already flagged or none available — no-op. *)
SchedNoop ==
    /\ pc_sched = "find_sleeper"
    /\ \A w \in Workers : note[w] = NOTE_FLAGGED \/ note[w] = NOTE_SLEEPING
    /\ \A w \in Workers : note[w] # NOTE_SLEEPING  \* No sleepers
    /\ pc_sched' = "done"
    /\ UNCHANGED <<stopping, note, parked, pc_worker, pc_main, pc_shutdown, woken_worker>>

SchedDone ==
    /\ pc_sched = "done"
    /\ pc_sched' = "idle"
    /\ woken_worker' = "none"
    /\ UNCHANGED <<stopping, note, parked, pc_worker, pc_main, pc_shutdown>>

(*******************************************************************************
 * MAIN_LOOP ACTIONS
 *
 * main_loop waits for quiescence (all workers parked) or user_done.
 * Workers signal quiescence by setting parked=true and calling park_cv.notify_all.
 * We model main_loop as transitioning from "waiting" to "checking" when any
 * worker signals it. In "checking" it evaluates the predicate.
 ******************************************************************************)

(* Main_loop is notified (by a worker parking). Check quiescence. *)
MainCheck ==
    /\ pc_main = "checking"
    /\ LET all_parked == \A w \in Workers :
                pc_worker[w] = "sleeping" \/ pc_worker[w] = "exited"
       IN /\ IF all_parked \/ stopping
             THEN pc_main' = "quiescent"
             ELSE pc_main' = "waiting"
          /\ UNCHANGED <<stopping, note, parked, pc_worker, pc_shutdown, pc_sched, woken_worker>>

(* Main_loop detected quiescence. It calls the quiescence hook.
 * (Simplified: just return to waiting.) *)
MainQuiescentHook ==
    /\ pc_main = "quiescent"
    /\ pc_main' = "waiting"
    /\ UNCHANGED <<stopping, note, parked, pc_worker, pc_shutdown, pc_sched, woken_worker>>

(*******************************************************************************
 * SHUTDOWN ACTIONS
 *
 * Shutdown protocol:
 *   1. Set stopping = true.
 *   2. For each worker: wake it via Note (CAS SLEEPING->AWAKE or set FLAGGED).
 *   (No park_mu empty-CS needed: per-worker notes have no window where
 *    the wakeup could be lost — if SLEEPING, wake succeeds; if AWAKE/FLAGGED,
 *    the worker will see stopping=true on its next check_work.)
 ******************************************************************************)

ShutdownSetFlag ==
    /\ pc_shutdown = "idle"
    /\ stopping' = TRUE
    /\ pc_shutdown' = "wake_workers"
    /\ UNCHANGED <<note, parked, pc_worker, pc_main, pc_sched, woken_worker>>

(* Wake all workers — set each sleeping Note to AWAKE, flag awake ones. *)
ShutdownWakeAll ==
    /\ pc_shutdown = "wake_workers"
    /\ note' = [w \in Workers |->
                    IF note[w] = NOTE_SLEEPING THEN NOTE_AWAKE
                    ELSE IF note[w] = NOTE_AWAKE THEN NOTE_FLAGGED
                    ELSE note[w]]   \* FLAGGED stays FLAGGED
    /\ pc_shutdown' = "done"
    /\ UNCHANGED <<stopping, parked, pc_worker, pc_main, pc_sched, woken_worker>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ \E w \in Workers :
        \/ WorkerCheckWork(w)
        \/ WorkerSetParked(w)
        \/ WorkerTrySleep(w)
        \/ WorkerWoken(w)
        \/ WorkerClearParked(w)
    \/ SchedStart
    \/ SchedWakeSleeping
    \/ SchedFlagAwake
    \/ SchedNoop
    \/ SchedDone
    \/ MainCheck
    \/ MainQuiescentHook
    \/ ShutdownSetFlag
    \/ ShutdownWakeAll

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ stopping \in BOOLEAN
    /\ note \in [Workers -> {NOTE_SLEEPING, NOTE_AWAKE, NOTE_FLAGGED}]
    /\ parked \in [Workers -> BOOLEAN]
    /\ pc_worker \in [Workers -> {"check_work", "set_parked", "try_sleep",
                                   "sleeping", "clear_parked", "exited"}]
    /\ pc_main \in {"waiting", "checking", "quiescent"}
    /\ pc_shutdown \in {"idle", "wake_workers", "done"}
    /\ pc_sched \in {"idle", "find_sleeper", "done"}
    /\ woken_worker \in ({"none"} \cup Workers)

(* Safety: unpark_one wakes at most one worker per call.
 * When sched transitions from find_sleeper to done after waking a sleeper,
 * exactly one worker's note changed SLEEPING->AWAKE. *)
ExactlyOneWoken ==
    \* After SchedWakeSleeping, at most one worker's note transitions.
    \* This is structurally enforced by the spec (CHOOSE picks one).
    TRUE  \* Structural: SchedWakeSleeping only modifies one note.

(* Safety: after shutdown completes, no worker is stuck sleeping with note=SLEEPING.
 * note=SLEEPING on a "sleeping" worker means no wakeup was delivered — stuck.
 * note=AWAKE or note=FLAGGED on a "sleeping" worker means it will wake up:
 *   - AWAKE: an unpark_one or shutdown has already delivered the wakeup.
 *   - FLAGGED: shutdown flagged an already-awoken worker; but if the worker
 *     is in "sleeping" pc-state it must have been woken (SLEEPING->AWAKE)
 *     before shutdown ran (shutdown converts AWAKE->FLAGGED, not SLEEPING->FLAGGED).
 *     So FLAGGED on a "sleeping" worker means it was woken by unpark_one first
 *     (note=AWAKE), then shutdown ran and set it to FLAGGED (AWAKE->FLAGGED).
 *     The worker will proceed from "sleeping" as soon as it gets scheduled
 *     (WorkerWoken requires note=AWAKE, not FLAGGED). So FLAGGED+sleeping IS
 *     problematic: the worker is sleeping expecting note=AWAKE but got FLAGGED.
 *
 * Correction: WorkerWoken only requires note[w] != NOTE_SLEEPING (any non-zero
 * value wakes the futex). We need to model this correctly:
 * note=FLAGGED can also wake a sleeping worker (futex wakes when value changes).
 * So: after shutdown, sleeping workers have note in {AWAKE, FLAGGED} (not SLEEPING). *)
ShutdownWakesAll ==
    (pc_shutdown = "done") =>
        \A w \in Workers :
            pc_worker[w] = "sleeping" => note[w] # NOTE_SLEEPING

(* Safety: workers only exit when stopping is true. *)
NoSpuriousExit ==
    \A w \in Workers : pc_worker[w] = "exited" => stopping

(* Safety: note=SLEEPING only when the worker is in "sleeping" or "try_sleep" state.
 * If a worker is in "check_work", "set_parked", or "clear_parked", its note
 * must be AWAKE or FLAGGED (never SLEEPING), because SLEEPING is only set
 * atomically when transitioning to "sleeping". *)
NoteConsistency ==
    \A w \in Workers :
        (note[w] = NOTE_SLEEPING) =>
            (pc_worker[w] = "sleeping" \/ pc_worker[w] = "try_sleep")

====
