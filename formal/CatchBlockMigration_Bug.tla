---- MODULE CatchBlockMigration_Bug ----
(*******************************************************************************
 * BUGGY version: channel send is INSIDE the catch block.
 *
 * spawn_entry (old code):
 *   try { f(); }
 *   catch (...) {
 *       auto ex = std::current_exception();
 *       sd->w << ex;    // <-- BUG: can yield inside catch
 *   }
 *   // __cxa_end_catch runs at } — but imp may have migrated threads
 *
 * The channel send (prialt_begin) can suspend the imp if no matching
 * reader exists.  When the imp resumes on a different OS thread,
 * __cxa_end_catch runs on that thread — but __cxa_begin_catch was
 * registered on the original thread's __cxa_eh_globals.  This corrupts
 * the C++ exception runtime state (SIGSEGV in __cxa_end_catch at 0x0).
 *
 * Expected result: TLC finds a violation of CatchBlockSameThread.
 ******************************************************************************)

EXTENDS Integers

VARIABLES
    worker_pc,
    worker_thread,
    sup_ready,
    catch_thread

vars == <<worker_pc, worker_thread, sup_ready, catch_thread>>

Threads == {"t1", "t2"}

(* Worker throws — enters catch block on current thread. *)
WorkerThrow ==
    /\ worker_pc = "running"
    /\ catch_thread' = worker_thread
    /\ worker_pc' = "in_catch"
    /\ UNCHANGED <<worker_thread, sup_ready>>

(* BUG: Worker sends INSIDE catch — supervisor ready, no yield. *)
WorkerSendInCatchReady ==
    /\ worker_pc = "in_catch"
    /\ sup_ready
    /\ worker_pc' = "catch_send_done"
    /\ UNCHANGED <<worker_thread, sup_ready, catch_thread>>

(* BUG: Worker sends INSIDE catch — supervisor not ready, must yield. *)
WorkerSendInCatchYield ==
    /\ worker_pc = "in_catch"
    /\ ~sup_ready
    /\ worker_pc' = "yielded_in_catch"
    /\ UNCHANGED <<worker_thread, sup_ready, catch_thread>>

(* Worker resumes on some thread — STILL inside catch block.
 * __cxa_end_catch will run on this (possibly different) thread. *)
WorkerResumeInCatch(t) ==
    /\ worker_pc = "yielded_in_catch"
    /\ sup_ready
    /\ worker_thread' = t
    /\ worker_pc' = "catch_send_done"
    /\ UNCHANGED <<sup_ready, catch_thread>>

(* __cxa_end_catch runs — but worker_thread may != catch_thread! *)
WorkerExitCatch ==
    /\ worker_pc = "catch_send_done"
    /\ worker_pc' = "done"
    /\ UNCHANGED <<worker_thread, sup_ready, catch_thread>>

(* Supervisor enters alt(). *)
SupervisorReady ==
    /\ ~sup_ready
    /\ sup_ready' = TRUE
    /\ UNCHANGED <<worker_pc, worker_thread, catch_thread>>

Init ==
    /\ worker_pc = "running"
    /\ worker_thread = "t1"
    /\ sup_ready = FALSE
    /\ catch_thread = "none"

Next ==
    \/ WorkerThrow
    \/ WorkerSendInCatchReady
    \/ WorkerSendInCatchYield
    \/ \E t \in Threads : WorkerResumeInCatch(t)
    \/ WorkerExitCatch
    \/ SupervisorReady

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ worker_pc \in {"running", "in_catch", "yielded_in_catch", "catch_send_done", "done"}
    /\ worker_thread \in Threads
    /\ sup_ready \in BOOLEAN
    /\ catch_thread \in Threads \cup {"none"}

(* This invariant SHOULD be violated: __cxa_end_catch can run on a
 * different thread when the catch block spans a yield.
 * Specifically, catch_send_done state can have worker_thread != catch_thread. *)
CatchBlockSameThread ==
    worker_pc \in {"in_catch", "yielded_in_catch", "catch_send_done"}
        => worker_thread = catch_thread

====
