---- MODULE SurplusProcessorReset_Bug ----
(*******************************************************************************
 * BUGGY VERSION — demonstrates the use-after-free race when a dead surplus
 * processor slot is reused by replacing the unique_ptr instead of resetting
 * in-place.
 *
 * The bug: add_processor does:
 *
 *   procs[idx] = std::make_unique<Processor>(idx);   // WRONG: destroys old!
 *
 * instead of the correct:
 *
 *   procs[idx]->reset();                             // correct: reset in-place
 *
 * Why this is dangerous:
 *
 * steal_work dereferences procs[idx] to a raw reference BEFORE acquiring
 * global_mu (see runtime.cpp line 313: "auto& victim = *procs[i]"):
 *
 *   auto& victim = *procs[i];          // dereference unique_ptr → raw ref
 *   if (&victim == &thief) continue;
 *   if (!victim.alive.load(acquire)) continue;  // use raw ref (no lock yet!)
 *   {
 *       std::unique_lock lk(victim.run_mu);     // still using raw ref
 *       std::unique_lock glk(global_mu, try_to_lock);
 *       ...
 *   }
 *
 * With unique_ptr replacement, between the dereference and the global_mu
 * acquisition, add_processor can destroy the old Processor object (via the
 * unique_ptr assignment). steal_work then holds a dangling reference.
 *
 * Dangerous interleaving:
 *   1. steal_work dereferences procs[idx] → victim (raw ref to old object).
 *   2. add_processor acquires global_mu, scans, joins old worker.
 *   3. add_processor calls make_unique<Processor> → OLD object FREED.
 *   4. steal_work reads victim.alive — USE-AFTER-FREE!
 *
 * With in-place reset: procs[idx] always points to the SAME Processor object.
 * steal_work's reference stays valid forever. The reset happens under global_mu,
 * and steal's field access also happens under global_mu — they're mutually
 * exclusive. The alive acquire/release then ensures fields are consistent.
 *
 * Expected result: TLC finds a NoPtrDerefWhenFreed violation.
 ******************************************************************************)

EXTENDS Integers

CONSTANTS
    P   \* The surplus processor slot being modeled

(*******************************************************************************
 * In the bug variant we split steal's ptr dereference from its alive check
 * to expose the window where the object is freed between those two steps.
 *
 * proc_state values:
 *   "dead"   — alive=false, old object exists but logically dead
 *   "freed"  — old unique_ptr destroyed; procs[idx] points to new (being stored)
 *   "alive"  — new object stored and alive=true
 *
 * ptr_valid: TRUE when the raw reference steal holds points to a live object.
 *   - TRUE initially (old object alive)
 *   - FALSE during "freed" (old object destroyed)
 *   - TRUE again once "alive" (new object stored)
 *
 * steal_work states:
 *   "idle"           — not running
 *   "deref_ptr"      — about to dereference procs[idx] (get raw victim ref)
 *   "load_alive"     — raw ref obtained; about to load victim.alive
 *   "try_global_mu"  — saw alive=true; trying try_to_lock(global_mu)
 *   "use_proc"       — holds global_mu; accessing processor fields
 *   "done"           — steal attempt finished
 ******************************************************************************)

VARIABLES
    proc_state,     \* "dead" | "freed" | "alive"
    proc_valid,     \* BOOLEAN: fields consistent (alive=true in new object)
    ptr_valid,      \* BOOLEAN: steal's raw ref points to a live object
    global_mu,      \* "none" | "watchdog" | "steal"
    pc_watchdog,    \* watchdog/add_processor pc
    pc_worker,      \* SurplusWorker pc
    pc_steal        \* StealWork pc

vars == <<proc_state, proc_valid, ptr_valid, global_mu, pc_watchdog, pc_worker, pc_steal>>

Init ==
    /\ proc_state = "alive"
    /\ proc_valid = TRUE
    /\ ptr_valid  = TRUE
    /\ global_mu  = "none"
    /\ pc_watchdog = "idle"
    /\ pc_worker   = "running"
    /\ pc_steal    = "idle"

(*******************************************************************************
 * SURPLUS WORKER ACTIONS
 ******************************************************************************)

WorkerWindDown ==
    /\ pc_worker = "running"
    /\ proc_state = "alive"
    /\ proc_state' = "dead"
    /\ pc_worker'  = "exited"
    /\ UNCHANGED <<proc_valid, ptr_valid, global_mu, pc_watchdog, pc_steal>>

(*******************************************************************************
 * WATCHDOG / add_processor ACTIONS (BUGGY: unique_ptr replacement)
 ******************************************************************************)

WatchdogAcquireMu ==
    /\ pc_watchdog = "idle"
    /\ global_mu = "none"
    /\ global_mu'  = "watchdog"
    /\ pc_watchdog' = "scan"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, pc_worker, pc_steal>>

WatchdogScan ==
    /\ pc_watchdog = "scan"
    /\ global_mu = "watchdog"
    /\ proc_state = "dead"
    /\ pc_watchdog' = "join"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, global_mu, pc_worker, pc_steal>>

WatchdogNoSlot ==
    /\ pc_watchdog = "scan"
    /\ global_mu = "watchdog"
    /\ proc_state # "dead"
    /\ global_mu'  = "none"
    /\ pc_watchdog' = "done"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, pc_worker, pc_steal>>

WatchdogJoin ==
    /\ pc_watchdog = "join"
    /\ global_mu = "watchdog"
    /\ pc_worker = "exited"
    /\ pc_watchdog' = "make_unique"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, global_mu, pc_worker, pc_steal>>

(* BUG STEP: make_unique<Processor> evaluates, then the unique_ptr assignment
 * DESTROYS the old object and stores the new pointer.
 *
 * The old Processor is freed here. Any concurrent steal_work that already
 * dereferenced procs[idx] (obtained a raw reference) now holds a dangling ref.
 * ptr_valid becomes FALSE to model this window.
 *
 * Code (buggy): procs[idx] = std::make_unique<Processor>(idx);
 *
 * TLA:SurplusProcessorReset_Bug.WatchdogMakeUnique *)
WatchdogMakeUnique ==
    /\ pc_watchdog = "make_unique"
    /\ global_mu = "watchdog"
    /\ proc_state = "dead"
    /\ proc_state' = "freed"
    /\ proc_valid'  = FALSE
    /\ ptr_valid'   = FALSE         \* old object destroyed; new not yet published
    /\ pc_watchdog' = "store_new"
    /\ UNCHANGED <<global_mu, pc_worker, pc_steal>>

(* New Processor stored and alive=true (set in constructor).
 * ptr_valid becomes TRUE again; the new object is valid.
 *
 * TLA:SurplusProcessorReset_Bug.WatchdogStoreNew *)
WatchdogStoreNew ==
    /\ pc_watchdog = "store_new"
    /\ global_mu = "watchdog"
    /\ proc_state = "freed"
    /\ proc_state' = "alive"
    /\ proc_valid'  = TRUE
    /\ ptr_valid'   = TRUE
    /\ pc_watchdog' = "spawn"
    /\ UNCHANGED <<global_mu, pc_worker, pc_steal>>

WatchdogSpawn ==
    /\ pc_watchdog = "spawn"
    /\ global_mu = "watchdog"
    /\ global_mu'  = "none"
    /\ pc_watchdog' = "done"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, pc_worker, pc_steal>>

(*******************************************************************************
 * STEAL_WORK ACTIONS
 *
 * The bug: steal dereferences procs[idx] to a raw reference BEFORE any lock.
 * If the watchdog replaces the unique_ptr (frees old object) while steal
 * holds a raw reference to the old object, steal's subsequent reads are
 * use-after-free.
 *
 * In the correct code (in-place reset), the object is never freed, so
 * the raw reference remains valid regardless of what the watchdog does.
 ******************************************************************************)

StealBegin ==
    /\ pc_steal = "idle"
    /\ pc_steal' = "deref_ptr"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, global_mu, pc_watchdog, pc_worker>>

(* steal_work dereferences procs[idx] to get a raw victim reference.
 * No lock held. ptr_valid must be TRUE at this point for correctness.
 *
 * Code: runtime.cpp line 313: auto& victim = *procs[i];
 *
 * TLA:SurplusProcessorReset_Bug.StealDerefPtr *)
StealDerefPtr ==
    /\ pc_steal = "deref_ptr"
    /\ pc_steal' = "load_alive"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, global_mu, pc_watchdog, pc_worker>>

(* steal_work loads victim.alive (acquire) via the raw reference.
 * No lock held. This is a USE-AFTER-FREE if ptr_valid=FALSE.
 *
 * Code: runtime.cpp line 315: if (!victim.alive.load(acquire)) continue
 *
 * TLA:SurplusProcessorReset_Bug.StealLoadAlive *)
StealLoadAlive ==
    /\ pc_steal = "load_alive"
    \* The model allows this step regardless of ptr_valid — the bug
    \* is precisely that steal does not check whether the pointer is valid.
    /\ IF proc_state = "alive"
       THEN pc_steal' = "try_global_mu"
       ELSE pc_steal' = "done"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, global_mu, pc_watchdog, pc_worker>>

StealTryGlobalMuOK ==
    /\ pc_steal = "try_global_mu"
    /\ global_mu = "none"
    /\ global_mu' = "steal"
    /\ pc_steal'  = "use_proc"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, pc_watchdog, pc_worker>>

StealTryGlobalMuFail ==
    /\ pc_steal = "try_global_mu"
    /\ global_mu # "none"
    /\ pc_steal' = "done"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, global_mu, pc_watchdog, pc_worker>>

StealUseProc ==
    /\ pc_steal = "use_proc"
    /\ global_mu = "steal"
    /\ global_mu' = "none"
    /\ pc_steal'  = "done"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, pc_watchdog, pc_worker>>

StealReset ==
    /\ pc_steal = "done"
    /\ pc_steal' = "idle"
    /\ UNCHANGED <<proc_state, proc_valid, ptr_valid, global_mu, pc_watchdog, pc_worker>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ WorkerWindDown
    \/ WatchdogAcquireMu
    \/ WatchdogScan
    \/ WatchdogNoSlot
    \/ WatchdogJoin
    \/ WatchdogMakeUnique
    \/ WatchdogStoreNew
    \/ WatchdogSpawn
    \/ StealBegin
    \/ StealDerefPtr
    \/ StealLoadAlive
    \/ StealTryGlobalMuOK
    \/ StealTryGlobalMuFail
    \/ StealUseProc
    \/ StealReset

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ proc_state \in {"dead", "freed", "alive"}
    /\ proc_valid \in BOOLEAN
    /\ ptr_valid  \in BOOLEAN
    /\ global_mu  \in {"none", "watchdog", "steal"}
    /\ pc_watchdog \in {"idle", "scan", "join",
                        "make_unique", "store_new", "spawn", "done"}
    /\ pc_worker   \in {"running", "exited"}
    /\ pc_steal    \in {"idle", "deref_ptr", "load_alive", "try_global_mu",
                        "use_proc", "done"}

(* Safety: steal_work only uses a processor's fields when proc_valid=TRUE. *)
StealOnlyUsesValidProc ==
    pc_steal = "use_proc" => proc_valid = TRUE

(* Safety: steal_work must not access a freed object.
 *
 * This invariant is violated in the bug variant: steal can be in load_alive
 * (or later) while the old object is freed (ptr_valid=FALSE), because
 * steal dereferenced the pointer before any lock was held and the watchdog
 * freed the object under global_mu.
 *
 * TLA:SurplusProcessorReset_Bug.NoPtrDerefWhenFreed *)
NoPtrDerefWhenFreed ==
    pc_steal \in {"load_alive", "try_global_mu", "use_proc"} => ptr_valid = TRUE

(* alive=true implies proc_valid. *)
AliveImpliesValid ==
    proc_state = "alive" => proc_valid = TRUE

====
