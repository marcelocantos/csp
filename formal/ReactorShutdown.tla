---- MODULE ReactorShutdown ----
(*******************************************************************************
 * Models the reactor shutdown protocol: the join-before-cleanup ordering
 * in Reactor::shutdown() that prevents concurrent fire_signal() and
 * writer map cleanup.
 *
 * The reactor loop thread calls fire_signal() which erases writers from
 * the signal maps under signal_mu.  Shutdown must join the reactor
 * thread BEFORE clearing the maps, otherwise fire_signal and the
 * cleanup race on the unordered_map (concurrent mutation = UB).
 *
 * Fix: shutdown() sets stopping_, wakes the loop, joins the thread,
 * THEN clears writers under signal_mu.
 *
 * Participants:
 *   Reactor — loop thread processing events and firing signals
 *   Shutdown — the shutdown() call (from scheduler or shutdown_runtime)
 *
 * Code references:
 *   ReactorLoop      — reactor.cc:167-182 (macOS) / 348-391 (Linux)
 *   ReactorFire      — reactor.cc:148-165 (fire_signal under signal_mu)
 *   ShutdownSetStop  — reactor.cc:58 (stopping_=true)
 *   ShutdownWake     — reactor.cc:59 (wake)
 *   ShutdownJoin     — reactor.cc:60 (thread_.join)
 *   ShutdownCleanup  — reactor.cc:65-71 (clear writers under signal_mu)
 *   ShutdownDone     — reactor.cc:73 (running_=false)
 *
 * Expected result: TLC finds no violations.
 ******************************************************************************)

EXTENDS Integers, FiniteSets

CONSTANTS
    Timers      \* set of timer IDs, e.g. {t1, t2}

VARIABLES
    stopping,           \* Boolean: reactor stopping flag
    loop_alive,         \* Boolean: reactor loop thread is running
    joined,             \* Boolean: thread has been joined
    timer_writers,      \* set of timer IDs with live writers
    pending_signals,    \* Int: pending signal count
    reactor_pc,         \* Reactor loop program counter
    shutdown_pc,        \* Shutdown program counter
    fire_batch          \* set of timer IDs ready to fire this batch

vars == <<stopping, loop_alive, joined, timer_writers, pending_signals,
          reactor_pc, shutdown_pc, fire_batch>>

(*******************************************************************************
 * Timer states:
 *   In timer_writers → writer exists, signal unfired
 *   Not in timer_writers → writer erased (fired or cleaned up)
 *
 * The reactor loop processes events in batches: dequeue all ready events,
 * fire each one.  stopping_ is only checked between batches.
 ******************************************************************************)

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
 * REACTOR LOOP ACTIONS
 ******************************************************************************)

(* Check stopping_ at top of loop.  If true, exit loop.
 * Code: while (!stopping_.load(acquire)) { ... }
 *
 * TLA:ReactorShutdown.ReactorCheckStop *)
ReactorCheckStop ==
    /\ reactor_pc = "check_stop"
    /\ loop_alive
    /\ IF stopping
       THEN /\ reactor_pc' = "loop_exited"
            /\ UNCHANGED <<stopping, loop_alive, joined, timer_writers,
                           pending_signals, shutdown_pc, fire_batch>>
       ELSE \* Dequeue a batch of ready events (non-deterministic subset).
            /\ \E batch \in SUBSET timer_writers :
                /\ fire_batch' = batch
                /\ reactor_pc' = IF batch = {} THEN "check_stop"
                                 ELSE "fire_one"
            /\ UNCHANGED <<stopping, loop_alive, joined, timer_writers,
                           pending_signals, shutdown_pc>>

(* Fire one signal from the current batch.
 * Erases writer under signal_mu, decrements pending_signals.
 *
 * Code: reactor.cc:148-165 (fire_signal)
 *
 * TLA:ReactorShutdown.ReactorFireOne *)
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

(* Reactor loop has exited — thread can now be joined.
 * TLA:ReactorShutdown.ReactorLoopExited *)
ReactorLoopExited ==
    /\ reactor_pc = "loop_exited"
    /\ loop_alive' = FALSE
    /\ reactor_pc' = "dead"
    /\ UNCHANGED <<stopping, joined, timer_writers, pending_signals,
                   shutdown_pc, fire_batch>>

(*******************************************************************************
 * SHUTDOWN ACTIONS
 ******************************************************************************)

(* Set stopping flag.
 * Code: reactor.cc:58 — stopping_.store(true, release)
 *
 * TLA:ReactorShutdown.ShutdownSetStop *)
ShutdownSetStop ==
    /\ shutdown_pc = "idle"
    /\ stopping' = TRUE
    /\ shutdown_pc' = "join"
    /\ UNCHANGED <<loop_alive, joined, timer_writers, pending_signals,
                   reactor_pc, fire_batch>>

(* Join reactor thread (waits for loop to exit).
 * Code: reactor.cc:59-60 — wake(); thread_.join();
 *
 * TLA:ReactorShutdown.ShutdownJoin *)
ShutdownJoin ==
    /\ shutdown_pc = "join"
    /\ ~loop_alive
    /\ joined' = TRUE
    /\ shutdown_pc' = "cleanup"
    /\ UNCHANGED <<stopping, loop_alive, timer_writers, pending_signals,
                   reactor_pc, fire_batch>>

(* Clear remaining writers under signal_mu.
 * Code: reactor.cc:65-71
 *
 * TLA:ReactorShutdown.ShutdownCleanup *)
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
    \/ ShutdownJoin
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
    /\ pending_signals \in 0..Cardinality(Timers)
    /\ reactor_pc \in {"check_stop", "fire_one", "loop_exited", "dead"}
    /\ shutdown_pc \in {"idle", "join", "cleanup", "done"}
    /\ fire_batch \subseteq Timers

(* Safety: cleanup only happens after join (no concurrent map mutation).
 * The reactor loop is guaranteed dead before writers are cleared. *)
NoRacyCleanup ==
    shutdown_pc = "cleanup" => ~loop_alive /\ joined

(* Safety: the reactor loop never fires a signal after being joined. *)
NoFireAfterJoin ==
    joined => reactor_pc \in {"loop_exited", "dead"}

(* Safety: pending_signals never goes negative. *)
NonNegativePending ==
    pending_signals >= 0

====
