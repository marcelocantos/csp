---- MODULE StackPoolReclamation_Bug ----
(*******************************************************************************
 * Bug variant of StackPoolReclamation.
 *
 * Defect: release(S) is called BEFORE do_switch completes — i.e., while
 * ImpA's fcontext is still live on S. This could happen if destroy_imp
 * mistakenly called release(S) before transferring control (e.g., freeing
 * the stack in the same execution context that is still running on it).
 *
 * In this variant, the scheduler calls release(S) from ImpA's stack
 * (imp_a_pc = "running"), then calls do_switch. The window between
 * SchedEarlyRelease and ImpASwitch allows SchedAllocate to hand S to ImpB
 * while ImpA is still running on it.
 *
 * Expected TLC result: invariant violation of NoDoubleAlloc.
 *   TLC will find a state where stack_state = "in_use_B" and imp_a_pc = "running".
 *
 * See also: docs/papers/25-stack-pool-reclamation.md
 ******************************************************************************)

EXTENDS Integers

VARIABLES
    stack_state,
    mu,
    imp_a_pc,
    scheduler_pc,
    imp_b_pc

vars == <<stack_state, mu, imp_a_pc, scheduler_pc, imp_b_pc>>

Init ==
    /\ stack_state = "in_use_A"
    /\ mu = "none"
    /\ imp_a_pc = "running"
    /\ scheduler_pc = "idle"
    /\ imp_b_pc = "waiting"

(*******************************************************************************
 * BUG: The scheduler calls release(S) while ImpA is still running on S
 * (scheduler_pc starts at "releasing" while imp_a_pc = "running").
 * This models a hypothetical incorrect implementation that frees the stack
 * before switching off it.
 ******************************************************************************)

(* BUG ACTION: Scheduler calls release while ImpA is still running.
 * In the correct implementation this is impossible because release() is called
 * from destroy_imp which runs on the SCHEDULER's stack, not on S.
 * Here we introduce the bug by allowing the release to happen early.
 *
 * TLA:StackPoolReclamation_Bug.SchedEarlyRelease *)
SchedEarlyRelease ==
    /\ scheduler_pc = "idle"
    /\ imp_a_pc = "running"         \* BUG: ImpA still live on S
    /\ mu = "none"
    /\ mu' = "scheduler"
    /\ scheduler_pc' = "releasing"
    /\ UNCHANGED <<stack_state, imp_a_pc, imp_b_pc>>

SchedRelease ==
    /\ scheduler_pc = "releasing"
    /\ mu = "scheduler"
    /\ stack_state = "in_use_A"     \* BUG: releasing while A is running
    /\ stack_state' = "in_pool"     \* S is now in pool while A is alive
    /\ mu' = "none"
    /\ scheduler_pc' = "released"
    /\ UNCHANGED <<imp_a_pc, imp_b_pc>>

(* ImpA calls do_switch AFTER the buggy early release.
 *
 * TLA:StackPoolReclamation_Bug.ImpASwitch *)
ImpASwitch ==
    /\ imp_a_pc = "running"
    /\ imp_a_pc' = "dead"
    /\ UNCHANGED <<stack_state, mu, scheduler_pc, imp_b_pc>>

SchedStartAlloc ==
    /\ scheduler_pc = "released"
    /\ scheduler_pc' = "allocating"
    /\ UNCHANGED <<stack_state, mu, imp_a_pc, imp_b_pc>>

SchedAcquireAllocMu ==
    /\ scheduler_pc = "allocating"
    /\ mu = "none"
    /\ mu' = "scheduler"
    /\ scheduler_pc' = "mu_alloc"
    /\ UNCHANGED <<stack_state, imp_a_pc, imp_b_pc>>

SchedAllocate ==
    /\ scheduler_pc = "mu_alloc"
    /\ mu = "scheduler"
    /\ stack_state = "in_pool"
    /\ stack_state' = "in_use_B"
    /\ mu' = "none"
    /\ scheduler_pc' = "allocated"
    /\ imp_b_pc' = "running"
    /\ UNCHANGED <<imp_a_pc>>

SchedDone ==
    /\ scheduler_pc = "allocated"
    /\ scheduler_pc' = "done"
    /\ UNCHANGED <<stack_state, mu, imp_a_pc, imp_b_pc>>

ImpBDone ==
    /\ imp_b_pc = "running"
    /\ imp_b_pc' = "done"
    /\ UNCHANGED <<stack_state, mu, imp_a_pc, scheduler_pc>>

Next ==
    \/ SchedEarlyRelease
    \/ SchedRelease
    \/ ImpASwitch
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
    /\ stack_state \in {"in_use_A", "in_pool", "in_use_B"}
    /\ mu \in {"none", "scheduler"}
    /\ imp_a_pc \in {"running", "dead"}
    /\ scheduler_pc \in {"idle", "releasing", "released",
                         "allocating", "mu_alloc", "allocated", "done"}
    /\ imp_b_pc \in {"waiting", "running", "done"}

(* This WILL be violated: ImpB runs on S while ImpA is still alive. *)
NoDoubleAlloc ==
    stack_state = "in_use_B" => imp_a_pc = "dead"

====
