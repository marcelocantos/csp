---- MODULE StackPoolReclamation ----
(*******************************************************************************
 * Models the stack pool alloc/release protocol from csp/src/stack_pool.cc.
 *
 * Safety concern: an imp's stack must not be returned to the pool (and
 * potentially reallocated) until the imp has fully switched off the stack.
 * The hazard would be: release(S) called while fcontext on S is still live,
 * then allocate() hands S to a new imp — two execution contexts racing on
 * the same memory.
 *
 * In the correct implementation this is prevented by ordering:
 *   1. do_switch: imp A transfers control to the scheduler's main stack.
 *      This is an irreversible, non-returning call from A's perspective.
 *      After do_switch the old fcontext on S is dead.
 *   2. destroy_imp: the scheduler (now on its own stack) calls release(S).
 *   3. allocate: a new imp B may receive S from the pool.
 *
 * Steps 1 → 2 → 3 are strictly ordered. Step 2 cannot precede step 1 because
 * destroy_imp runs on the scheduler stack, not on S.
 *
 * Participants:
 *   ImpA      — the exiting imp whose stack S transitions through states
 *   Scheduler — calls destroy_imp after the switch; also allocates stacks
 *   ImpB      — a new imp that may be allocated stack S
 *
 * Stack states:
 *   "in_use_A"   — S is allocated to ImpA; ImpA's fcontext is live on S
 *   "in_transit" — do_switch called; ImpA's fcontext is dead; destroy_imp
 *                  has not yet called release(S)
 *   "in_pool"    — S is in the free list; available for allocation
 *   "in_use_B"   — S has been allocated to ImpB
 *
 * Code references (src/stack_pool.cc, src/csp.cc):
 *   DoSwitch    — csp.cc: do_switch(Status::detach) from destroy_imp path
 *   Release     — stack_pool.cc:95-107 arena_free (acquire mu_, push free_list_)
 *   Allocate    — stack_pool.cc:53-93 arena_alloc (acquire mu_, pop free_list_)
 *
 * Key invariant verified:
 *   NoDoubleAlloc — the stack is never simultaneously in_pool and in_use_*
 *   NoUseAfterFree — ImpB can only use S after ImpA has switched off
 *
 * See also: docs/papers/24-stack-pool-reclamation.md
 ******************************************************************************)

EXTENDS Integers

VARIABLES
    stack_state,    \* "in_use_A" | "in_transit" | "in_pool" | "in_use_B"
    mu,             \* "none" | "scheduler" — who holds pool mutex
    imp_a_pc,       \* ImpA program counter
    scheduler_pc,   \* scheduler/destroy_imp program counter
    imp_b_pc        \* ImpB program counter

vars == <<stack_state, mu, imp_a_pc, scheduler_pc, imp_b_pc>>

(*******************************************************************************
 * Program counters:
 *
 * ImpA:
 *   "running"    — ImpA executing on stack S
 *   "switching"  — ImpA calls do_switch; this is the irreversible transfer
 *   "dead"       — do_switch has completed; ImpA is gone
 *
 * Scheduler (runs on its own main stack, never on S):
 *   "idle"       — waiting for an imp to exit
 *   "releasing"  — about to acquire pool mu and call release(S)
 *   "mu_held"    — pool mu acquired; about to push S onto free list
 *   "released"   — S is back in pool; scheduler may now allocate for ImpB
 *   "allocating" — about to acquire pool mu to allocate S for ImpB
 *   "mu_alloc"   — pool mu acquired; about to pop S from free list
 *   "allocated"  — S handed to ImpB
 *   "done"       — cycle complete
 *
 * ImpB:
 *   "waiting"    — not yet spawned
 *   "running"    — running on stack S
 *   "done"       — finished
 ******************************************************************************)

Init ==
    /\ stack_state = "in_use_A"
    /\ mu = "none"
    /\ imp_a_pc = "running"
    /\ scheduler_pc = "idle"
    /\ imp_b_pc = "waiting"

(*******************************************************************************
 * IMP A ACTIONS
 ******************************************************************************)

(* ImpA calls do_switch: transfers control to the scheduler's main stack.
 * This is the irreversible abandonment of stack S. After this step,
 * ImpA's fcontext on S is dead.
 *
 * Code: do_switch(Status::detach) in destroy_imp path
 *
 * TLA:StackPoolReclamation.ImpASwitch *)
ImpASwitch ==
    /\ imp_a_pc = "running"
    /\ stack_state = "in_use_A"
    /\ stack_state' = "in_transit"     \* fcontext on S is now dead
    /\ imp_a_pc' = "switching"
    /\ scheduler_pc' = "releasing"     \* scheduler wakes up to run destroy_imp
    /\ UNCHANGED <<mu, imp_b_pc>>

(* do_switch completes: ImpA is fully gone. *)
ImpADead ==
    /\ imp_a_pc = "switching"
    /\ imp_a_pc' = "dead"
    /\ UNCHANGED <<stack_state, mu, scheduler_pc, imp_b_pc>>

(*******************************************************************************
 * SCHEDULER ACTIONS (destroy_imp path + allocation)
 *
 * The scheduler runs on its own stack. It calls release(S) then, separately,
 * allocate() to get a stack for ImpB.
 ******************************************************************************)

(* Scheduler acquires pool mu to release S.
 *
 * Code: std::lock_guard<std::mutex> lk(mu_)  in arena_free
 *
 * TLA:StackPoolReclamation.SchedAcquireRelMu *)
SchedAcquireRelMu ==
    /\ scheduler_pc = "releasing"
    /\ mu = "none"
    /\ mu' = "scheduler"
    /\ scheduler_pc' = "mu_held"
    /\ UNCHANGED <<stack_state, imp_a_pc, imp_b_pc>>

(* Scheduler pushes S onto the free list and releases mu.
 * Combined: push + mu release to keep the model compact.
 *
 * Code: free_list_.push_back(region); [mu released at scope exit]
 *
 * TLA:StackPoolReclamation.SchedRelease *)
SchedRelease ==
    /\ scheduler_pc = "mu_held"
    /\ mu = "scheduler"
    /\ stack_state = "in_transit"
    /\ stack_state' = "in_pool"
    /\ mu' = "none"
    /\ scheduler_pc' = "released"
    /\ UNCHANGED <<imp_a_pc, imp_b_pc>>

(* Scheduler decides to allocate stack S for ImpB. *)
SchedStartAlloc ==
    /\ scheduler_pc = "released"
    /\ scheduler_pc' = "allocating"
    /\ UNCHANGED <<stack_state, mu, imp_a_pc, imp_b_pc>>

(* Scheduler acquires pool mu to allocate.
 *
 * Code: std::lock_guard<std::mutex> lk(mu_)  in arena_alloc
 *
 * TLA:StackPoolReclamation.SchedAcquireAllocMu *)
SchedAcquireAllocMu ==
    /\ scheduler_pc = "allocating"
    /\ mu = "none"
    /\ mu' = "scheduler"
    /\ scheduler_pc' = "mu_alloc"
    /\ UNCHANGED <<stack_state, imp_a_pc, imp_b_pc>>

(* Scheduler pops S from free list, releases mu, hands to ImpB.
 *
 * Code: auto region = free_list_.back(); free_list_.pop_back(); [mu released]
 *
 * TLA:StackPoolReclamation.SchedAllocate *)
SchedAllocate ==
    /\ scheduler_pc = "mu_alloc"
    /\ mu = "scheduler"
    /\ stack_state = "in_pool"
    /\ stack_state' = "in_use_B"
    /\ mu' = "none"
    /\ scheduler_pc' = "allocated"
    /\ imp_b_pc' = "running"
    /\ UNCHANGED <<imp_a_pc>>

(* Scheduler marks the cycle done. *)
SchedDone ==
    /\ scheduler_pc = "allocated"
    /\ scheduler_pc' = "done"
    /\ UNCHANGED <<stack_state, mu, imp_a_pc, imp_b_pc>>

(*******************************************************************************
 * IMP B ACTIONS
 ******************************************************************************)

(* ImpB runs on stack S and eventually finishes. *)
ImpBDone ==
    /\ imp_b_pc = "running"
    /\ imp_b_pc' = "done"
    /\ UNCHANGED <<stack_state, mu, imp_a_pc, scheduler_pc>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ ImpASwitch
    \/ ImpADead
    \/ SchedAcquireRelMu
    \/ SchedRelease
    \/ SchedStartAlloc
    \/ SchedAcquireAllocMu
    \/ SchedAllocate
    \/ SchedDone
    \/ ImpBDone

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

TypeOK ==
    /\ stack_state \in {"in_use_A", "in_transit", "in_pool", "in_use_B"}
    /\ mu \in {"none", "scheduler"}
    /\ imp_a_pc \in {"running", "switching", "dead"}
    /\ scheduler_pc \in {"idle", "releasing", "mu_held", "released",
                         "allocating", "mu_alloc", "allocated", "done"}
    /\ imp_b_pc \in {"waiting", "running", "done"}

(* Safety: the stack is never simultaneously allocated to two imps.
 * ImpA's fcontext is live only when imp_a_pc = "running" AND
 * stack_state = "in_use_A" (or "switching" — do_switch is in progress).
 * ImpB's fcontext is live only when imp_b_pc = "running".
 * These must be mutually exclusive. *)
NoDoubleAlloc ==
    \* Stack cannot be in_use_A and in_use_B at the same time
    stack_state \in {"in_use_A", "in_transit", "in_pool", "in_use_B"}
    /\ ~(stack_state = "in_use_B" /\ imp_a_pc = "running")
    /\ ~(imp_b_pc = "running" /\ imp_a_pc = "running")

(* Safety: ImpB can only be running on S after ImpA has switched off.
 * Specifically: once stack_state = "in_use_B", ImpA must be "dead" or
 * at least "switching" (do_switch has been called, fcontext is dead). *)
NoUseAfterFree ==
    stack_state = "in_use_B" => imp_a_pc \in {"switching", "dead"}

(* Safety: the pool mutex is never held while a fresh allocation
 * races with an active release — i.e., S is only popped from the pool
 * when it is actually in_pool. *)
AllocFromPool ==
    scheduler_pc = "mu_alloc" => stack_state = "in_pool"

(* Safety: release only happens after ImpA has switched off. *)
ReleaseAfterSwitch ==
    scheduler_pc \in {"mu_held", "released", "allocating", "mu_alloc",
                      "allocated", "done"}
    => imp_a_pc \in {"switching", "dead"}

====
