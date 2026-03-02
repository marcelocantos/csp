---- MODULE CatchBlockMigration ----
(*******************************************************************************
 * Models the interaction between C++ exception handling (__cxa_begin_catch /
 * __cxa_end_catch) and M:N imp context-switching during a channel send
 * in spawn_entry's catch block.
 *
 * Bug: spawn_entry performed a channel send INSIDE the catch block.
 * The channel send can suspend the imp (prialt_begin → no matching reader →
 * yield).  In M:N mode, the imp may resume on a different OS thread.
 * __cxa_end_catch then runs on thread B while __cxa_begin_catch registered
 * the exception on thread A's __cxa_eh_globals — corrupting the C++ runtime.
 *
 * Fix: move the channel send OUTSIDE the catch block.
 * std::exception_ptr is captured inside the catch, but the send that can
 * yield happens after __cxa_end_catch has already run on the original thread.
 *
 * Participants:
 *   Worker     — spawned imp that throws, catches, and sends exception_ptr
 *   Supervisor — imp waiting in alt() to receive the exception_ptr
 *   Threads    — OS threads {t1, t2} where imps can execute
 *
 * Code references:
 *   include/csp/csp.h:886-897  spawn_entry (fixed: send outside catch)
 *   src/csp.cc:317-320          prialt_begin → suspend flow
 *
 * Expected result: TLC finds no violations.
 ******************************************************************************)

EXTENDS Integers

VARIABLES
    worker_pc,      \* Worker program counter
    worker_thread,  \* OS thread the worker is currently on
    sup_ready,      \* Whether supervisor is ready to receive (in alt)
    catch_thread    \* Thread where __cxa_begin_catch was called

vars == <<worker_pc, worker_thread, sup_ready, catch_thread>>

Threads == {"t1", "t2"}

(*******************************************************************************
 * WORKER STEPS (fixed code path)
 *
 * The worker imp executes spawn_entry with the FIXED code:
 *   try { f(); }
 *   catch (...) { ex = std::current_exception(); }
 *   // __cxa_end_catch runs here, on same thread as begin_catch
 *   if (ex) { sd->w << ex; }  // may yield
 ******************************************************************************)

(* Worker throws — enters catch block on current thread.
 * __cxa_begin_catch registers on this thread's __cxa_eh_globals. *)
WorkerThrow ==
    /\ worker_pc = "running"
    /\ catch_thread' = worker_thread
    /\ worker_pc' = "in_catch"
    /\ UNCHANGED <<worker_thread, sup_ready>>

(* Worker captures exception_ptr and exits catch block.
 * __cxa_end_catch runs on same thread as __cxa_begin_catch. *)
WorkerExitCatch ==
    /\ worker_pc = "in_catch"
    /\ worker_pc' = "after_catch"
    /\ UNCHANGED <<worker_thread, sup_ready, catch_thread>>

(* Worker begins channel send (prialt_begin).
 * If supervisor is ready, send completes immediately — no yield. *)
WorkerSendReady ==
    /\ worker_pc = "after_catch"
    /\ sup_ready
    /\ worker_pc' = "done"
    /\ UNCHANGED <<worker_thread, sup_ready, catch_thread>>

(* Worker begins channel send but supervisor isn't ready — yield.
 * The imp suspends and will resume (possibly on a different thread). *)
WorkerSendYield ==
    /\ worker_pc = "after_catch"
    /\ ~sup_ready
    /\ worker_pc' = "yielded"
    /\ UNCHANGED <<worker_thread, sup_ready, catch_thread>>

(* Worker resumes on some thread after yield.
 * In M:N mode, this can be any thread — not necessarily the original. *)
WorkerResume(t) ==
    /\ worker_pc = "yielded"
    /\ sup_ready      \* supervisor must now be ready (it entered alt)
    /\ worker_thread' = t
    /\ worker_pc' = "done"
    /\ UNCHANGED <<sup_ready, catch_thread>>

(*******************************************************************************
 * SUPERVISOR
 *
 * Enters alt() at some point, making itself ready to receive.
 ******************************************************************************)

SupervisorReady ==
    /\ ~sup_ready
    /\ sup_ready' = TRUE
    /\ UNCHANGED <<worker_pc, worker_thread, catch_thread>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Init ==
    /\ worker_pc = "running"
    /\ worker_thread = "t1"
    /\ sup_ready = FALSE
    /\ catch_thread = "none"

Next ==
    \/ WorkerThrow
    \/ WorkerExitCatch
    \/ WorkerSendReady
    \/ WorkerSendYield
    \/ \E t \in Threads : WorkerResume(t)
    \/ SupervisorReady

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ worker_pc \in {"running", "in_catch", "after_catch", "yielded", "done"}
    /\ worker_thread \in Threads
    /\ sup_ready \in BOOLEAN
    /\ catch_thread \in Threads \cup {"none"}

(* Safety: __cxa_end_catch must run on the same thread as __cxa_begin_catch.
 * In the fixed code, __cxa_end_catch runs at the "after_catch" transition,
 * which happens before any yield.  So worker_thread == catch_thread at
 * that point.  This invariant checks that the catch block never spans
 * a thread migration — i.e., while in_catch, the thread doesn't change. *)
CatchBlockSameThread ==
    worker_pc = "in_catch" => worker_thread = catch_thread

====
