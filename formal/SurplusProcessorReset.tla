---- MODULE SurplusProcessorReset ----
(*******************************************************************************
 * Models the surplus processor add/wind-down/reuse protocol from
 * csp/src/runtime.cpp and include/csp/internal/processor.h.
 *
 * The protocol ensures that when a dead surplus processor slot is reused,
 * steal_work never observes a processor in a partially-reset state.
 *
 * Two orthogonal protections work together:
 *
 * 1. alive acquire/release ordering: reset() writes alive=true LAST with
 *    release. Any steal_work that observes alive=true with acquire has a
 *    happens-before edge guaranteeing all prior reset() writes are visible.
 *    This covers a NEW processor slot (no previous user).
 *
 * 2. global_mu exclusion during steal: steal_work holds global_mu (via
 *    try_to_lock) while dereferencing the victim's fields. add_processor
 *    also holds global_mu while resetting. So reset() and steal's field
 *    access are mutually exclusive. This covers REUSE of a formerly-alive
 *    slot: steal may have observed alive=true before wind-down, but if it
 *    reaches the field-access phase, reset is blocked on global_mu.
 *
 * Participants:
 *   Watchdog      — watchdog_loop thread; calls add_processor when a P stalls
 *   SurplusWorker — the worker thread running on the surplus P that winds down
 *   StealWork     — another worker thread calling steal_work
 *
 * Shared state:
 *   alive[p]      — std::atomic<bool>, written release / read acquire
 *                   (processor.h:21, reset():44, worker wind-down:186)
 *   proc_valid[p] — logical flag: all Processor fields other than alive are
 *                   consistent and safe to use. Set by reset() before alive.
 *   global_mu     — mutex held by add_processor and by steal while using fields
 *
 * Code references (src/runtime.cpp):
 *   SurplusWindDown   — lines 183-188: alive.store(false, release); return
 *   AddProcScanSlots  — lines 234-243: scan for dead surplus slot under global_mu
 *   AddProcJoin       — lines 237-239: join old worker thread
 *   AddProcReset      — line 248:      procs[idx]->reset() in-place
 *   AddProcSpawn      — lines 256-260: spawn new worker thread
 *   ResetClearFields  — processor.h:37-42: clear fields (relaxed stores)
 *   ResetSetAlive     — processor.h:44:    alive.store(true, release) — last
 *   StealLoadAlive    — line 315: if (!victim.alive.load(acquire)) continue
 *   StealTryGlobalMu  — line 324: try_to_lock(global_mu)
 *   StealUseProc      — lines 327-346: dereference victim fields under global_mu
 *
 * Safety property:
 *   StealWork only uses a processor's fields while holding global_mu.
 *   reset() only modifies fields while holding global_mu.
 *   Therefore the two are mutually exclusive — steal never sees mid-reset state.
 *
 * The bug variant (SurplusProcessorReset_Bug) replaces in-place reset with
 * unique_ptr replacement, freeing the old object while steal_work holds a
 * raw reference to it (before the global_mu try_to_lock).
 ******************************************************************************)

EXTENDS Integers

CONSTANTS
    P   \* The surplus processor slot being modeled (a single slot suffices)

(*******************************************************************************
 * Processor state values:
 *   "dead"      — alive=false, fields may be stale (worker has exited)
 *   "resetting" — reset() in progress: fields being cleared (global_mu held)
 *   "alive"     — alive=true (release), fields valid
 *
 * global_mu holders:
 *   "none"     — not held
 *   "watchdog" — held by add_processor
 *   "steal"    — held by steal_work (try_to_lock succeeded)
 *
 * steal_work states:
 *   "idle"          — not running
 *   "load_alive"    — about to load victim.alive (acquire); no lock held
 *   "try_global_mu" — trying try_to_lock(global_mu) after seeing alive=true
 *   "use_proc"      — holds global_mu, accessing processor fields
 *   "done"          — steal attempt finished, global_mu released
 *
 * Watchdog/add_processor states:
 *   "idle"       — not running
 *   "scan"       — scanning surplus slots (global_mu held)
 *   "join"       — joining old worker thread (global_mu held)
 *   "reset"      — calling reset(): clearing fields (global_mu held)
 *   "reset_alive"— reset(): about to store alive=true (global_mu held)
 *   "spawn"      — spawning new worker thread (global_mu held)
 *   "done"       — finished, global_mu released
 *
 * SurplusWorker states:
 *   "running" — executing normally
 *   "exited"  — alive=false stored, thread returned
 ******************************************************************************)

VARIABLES
    proc_state,     \* "dead" | "resetting" | "alive"
    proc_valid,     \* BOOLEAN: non-alive fields are consistent
    global_mu,      \* "none" | "watchdog" | "steal"
    pc_watchdog,    \* watchdog/add_processor program counter
    pc_worker,      \* SurplusWorker program counter
    pc_steal        \* StealWork program counter

vars == <<proc_state, proc_valid, global_mu, pc_watchdog, pc_worker, pc_steal>>

(*******************************************************************************
 * Initial state: surplus P is alive and running.
 ******************************************************************************)
Init ==
    /\ proc_state = "alive"
    /\ proc_valid = TRUE
    /\ global_mu  = "none"
    /\ pc_watchdog = "idle"
    /\ pc_worker   = "running"
    /\ pc_steal    = "idle"

(*******************************************************************************
 * SURPLUS WORKER ACTIONS
 *
 * Code: runtime.cpp lines 183-188
 ******************************************************************************)

(* Worker times out and stores alive=false with release, then exits.
 *
 * Code: runtime.cpp line 186: p.alive.store(false, memory_order_release)
 *
 * TLA:SurplusProcessorReset.WorkerWindDown *)
WorkerWindDown ==
    /\ pc_worker = "running"
    /\ proc_state = "alive"
    /\ proc_state' = "dead"
    /\ pc_worker'  = "exited"
    /\ UNCHANGED <<proc_valid, global_mu, pc_watchdog, pc_steal>>

(*******************************************************************************
 * WATCHDOG / add_processor ACTIONS
 *
 * add_processor holds global_mu throughout scan + join + reset.
 *
 * Code: runtime.cpp lines 229-264
 ******************************************************************************)

(* Watchdog acquires global_mu.
 *
 * Code: runtime.cpp line 230: std::lock_guard<std::mutex> lk(global_mu)
 *
 * TLA:SurplusProcessorReset.WatchdogAcquireMu *)
WatchdogAcquireMu ==
    /\ pc_watchdog = "idle"
    /\ global_mu = "none"
    /\ global_mu'  = "watchdog"
    /\ pc_watchdog' = "scan"
    /\ UNCHANGED <<proc_state, proc_valid, pc_worker, pc_steal>>

(* Watchdog scans and finds a dead surplus slot.
 *
 * Code: runtime.cpp lines 234-243
 *
 * TLA:SurplusProcessorReset.WatchdogScan *)
WatchdogScan ==
    /\ pc_watchdog = "scan"
    /\ global_mu = "watchdog"
    /\ proc_state = "dead"
    /\ pc_watchdog' = "join"
    /\ UNCHANGED <<proc_state, proc_valid, global_mu, pc_worker, pc_steal>>

(* No reusable slot — bail out and release mu.
 *
 * TLA:SurplusProcessorReset.WatchdogNoSlot *)
WatchdogNoSlot ==
    /\ pc_watchdog = "scan"
    /\ global_mu = "watchdog"
    /\ proc_state # "dead"
    /\ global_mu'  = "none"
    /\ pc_watchdog' = "done"
    /\ UNCHANGED <<proc_state, proc_valid, pc_worker, pc_steal>>

(* Watchdog joins old worker thread.
 *
 * join() returns only after the thread has exited. This provides a
 * happens-before edge: all writes by the old worker (including alive=false
 * with release) are visible to the watchdog after join returns.
 *
 * Code: runtime.cpp lines 237-239: procs[i]->worker.join()
 *
 * TLA:SurplusProcessorReset.WatchdogJoin *)
WatchdogJoin ==
    /\ pc_watchdog = "join"
    /\ global_mu = "watchdog"
    /\ pc_worker = "exited"     \* join() blocks until thread exits
    /\ pc_watchdog' = "reset"
    /\ UNCHANGED <<proc_state, proc_valid, global_mu, pc_worker, pc_steal>>

(* reset() step 1: clear all fields except alive (relaxed stores).
 * proc_valid becomes FALSE to model the transient inconsistency.
 *
 * This step is safe because:
 * - The old worker has exited (joined above).
 * - global_mu is held, so no concurrent steal is in use_proc.
 * - alive is still false, so no steal will proceed past load_alive.
 *
 * Code: processor.h lines 37-42
 *
 * TLA:SurplusProcessorReset.ResetClearFields *)
ResetClearFields ==
    /\ pc_watchdog = "reset"
    /\ global_mu = "watchdog"
    /\ proc_state = "dead"
    /\ proc_state' = "resetting"
    /\ proc_valid'  = FALSE
    /\ pc_watchdog' = "reset_alive"
    /\ UNCHANGED <<global_mu, pc_worker, pc_steal>>

(* reset() step 2: store alive=true with release — this is the LAST write.
 * After this step the processor is fully initialized and stealable.
 *
 * The release store ensures that any subsequent acquire load of alive=true
 * will see the completed reset (proc_valid=TRUE).
 *
 * Code: processor.h line 44: alive.store(true, memory_order_release)
 *
 * TLA:SurplusProcessorReset.ResetSetAlive *)
ResetSetAlive ==
    /\ pc_watchdog = "reset_alive"
    /\ global_mu = "watchdog"
    /\ proc_state = "resetting"
    /\ proc_state' = "alive"
    /\ proc_valid'  = TRUE
    /\ pc_watchdog' = "spawn"
    /\ UNCHANGED <<global_mu, pc_worker, pc_steal>>

(* Watchdog spawns new worker and releases global_mu.
 *
 * Code: runtime.cpp lines 256-264
 *
 * TLA:SurplusProcessorReset.WatchdogSpawn *)
WatchdogSpawn ==
    /\ pc_watchdog = "spawn"
    /\ global_mu = "watchdog"
    /\ global_mu'  = "none"
    /\ pc_watchdog' = "done"
    /\ UNCHANGED <<proc_state, proc_valid, pc_worker, pc_steal>>

(*******************************************************************************
 * STEAL_WORK ACTIONS
 *
 * steal_work protocol:
 *   1. Load alive with acquire (no lock held).
 *      If alive=false: skip this victim.
 *   2. Acquire victim.run_mu (not modeled here — orthogonal).
 *   3. Try to acquire global_mu (try_to_lock, non-blocking).
 *      If fails: continue to next victim (skip).
 *   4. Use processor fields while holding global_mu.
 *   5. Release global_mu (and run_mu).
 *
 * The key point: steal accesses processor fields ONLY while holding global_mu.
 * reset() also holds global_mu. So they cannot overlap.
 *
 * Code: runtime.cpp lines 310-354
 ******************************************************************************)

(* steal_work begins a steal attempt.
 *
 * TLA:SurplusProcessorReset.StealBegin *)
StealBegin ==
    /\ pc_steal = "idle"
    /\ pc_steal' = "load_alive"
    /\ UNCHANGED <<proc_state, proc_valid, global_mu, pc_watchdog, pc_worker>>

(* steal_work loads alive with acquire. No lock held at this point.
 *
 * If alive=false: skip. If alive=true: try to acquire global_mu.
 *
 * Code: runtime.cpp line 315: if (!victim.alive.load(acquire)) continue
 *
 * TLA:SurplusProcessorReset.StealLoadAlive *)
StealLoadAlive ==
    /\ pc_steal = "load_alive"
    /\ IF proc_state = "alive"
       THEN pc_steal' = "try_global_mu"
       ELSE pc_steal' = "done"
    /\ UNCHANGED <<proc_state, proc_valid, global_mu, pc_watchdog, pc_worker>>

(* steal_work tries to acquire global_mu (non-blocking try_to_lock).
 *
 * If global_mu is free: acquire it and proceed to use_proc.
 * If global_mu is held (by watchdog): skip this victim.
 *
 * Code: runtime.cpp line 324: std::unique_lock<std::mutex> glk(global_mu, try_to_lock)
 *                              if (!glk) continue;
 *
 * TLA:SurplusProcessorReset.StealTryGlobalMuOK *)
StealTryGlobalMuOK ==
    /\ pc_steal = "try_global_mu"
    /\ global_mu = "none"
    /\ global_mu' = "steal"
    /\ pc_steal'  = "use_proc"
    /\ UNCHANGED <<proc_state, proc_valid, pc_watchdog, pc_worker>>

(* global_mu is held — steal skips this victim.
 *
 * TLA:SurplusProcessorReset.StealTryGlobalMuFail *)
StealTryGlobalMuFail ==
    /\ pc_steal = "try_global_mu"
    /\ global_mu # "none"
    /\ pc_steal' = "done"
    /\ UNCHANGED <<proc_state, proc_valid, global_mu, pc_watchdog, pc_worker>>

(* steal_work uses processor fields while holding global_mu.
 * Releases global_mu when done.
 *
 * Code: runtime.cpp lines 327-347
 *
 * TLA:SurplusProcessorReset.StealUseProc *)
StealUseProc ==
    /\ pc_steal = "use_proc"
    /\ global_mu = "steal"
    /\ global_mu' = "none"
    /\ pc_steal'  = "done"
    /\ UNCHANGED <<proc_state, proc_valid, pc_watchdog, pc_worker>>

(* steal_work resets for next iteration.
 *
 * TLA:SurplusProcessorReset.StealReset *)
StealReset ==
    /\ pc_steal = "done"
    /\ pc_steal' = "idle"
    /\ UNCHANGED <<proc_state, proc_valid, global_mu, pc_watchdog, pc_worker>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ WorkerWindDown
    \/ WatchdogAcquireMu
    \/ WatchdogScan
    \/ WatchdogNoSlot
    \/ WatchdogJoin
    \/ ResetClearFields
    \/ ResetSetAlive
    \/ WatchdogSpawn
    \/ StealBegin
    \/ StealLoadAlive
    \/ StealTryGlobalMuOK
    \/ StealTryGlobalMuFail
    \/ StealUseProc
    \/ StealReset

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

(* Type invariant. *)
TypeOK ==
    /\ proc_state \in {"dead", "resetting", "alive"}
    /\ proc_valid \in BOOLEAN
    /\ global_mu  \in {"none", "watchdog", "steal"}
    /\ pc_watchdog \in {"idle", "scan", "join",
                        "reset", "reset_alive", "spawn", "done"}
    /\ pc_worker   \in {"running", "exited"}
    /\ pc_steal    \in {"idle", "load_alive", "try_global_mu",
                        "use_proc", "done"}

(* Safety: steal_work only uses a processor's fields when proc_valid=TRUE.
 *
 * Proof sketch:
 *   - steal reaches use_proc only after try_to_lock(global_mu) succeeds.
 *   - reset() sets proc_valid=FALSE under global_mu, then proc_valid=TRUE
 *     (via alive=true) still under global_mu.
 *   - While steal holds global_mu (in use_proc), reset cannot hold global_mu.
 *   - The only way proc_valid=FALSE exists is during "resetting" (global_mu=watchdog).
 *   - So when steal holds global_mu (global_mu="steal"), proc_state#"resetting"
 *     and thus proc_valid=TRUE.
 *   - Additionally, steal only reaches use_proc via alive=true, so proc_state="alive"
 *     at the moment of load; wind-down can happen concurrently but then
 *     watchdog must join before resetting, and joining blocks on worker exit.
 *
 * TLA:SurplusProcessorReset.StealOnlyUsesValidProc *)
StealOnlyUsesValidProc ==
    pc_steal = "use_proc" => proc_valid = TRUE

(* Safety: during resetting, proc_valid=FALSE (documents the transient window). *)
ResettingImpliesNotValid ==
    proc_state = "resetting" => proc_valid = FALSE

(* Safety: alive implies proc_valid (reset publishes valid state via alive). *)
AliveImpliesValid ==
    proc_state = "alive" => proc_valid = TRUE

(* Safety: global_mu exclusion — reset and steal never overlap. *)
ResetAndStealMutuallyExclusive ==
    ~(pc_watchdog \in {"reset", "reset_alive"} /\ pc_steal = "use_proc")

(* Safety: watchdog joins only after worker has exited. *)
JoinAfterExit ==
    pc_watchdog = "join" => pc_worker = "exited"

====
