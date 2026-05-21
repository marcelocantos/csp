# Paper 24: Stack Pool Reclamation Race

## Context

Imps run on fcontext stacks allocated from `StackPool`. When an imp finishes,
`destroy_imp` returns its stack to the pool via `StackPool::release`. The pool
is protected by a single mutex `mu_` — allocation (`allocate`) and deallocation
(`release`) both acquire it. The hazard is:

**An imp is still executing on a stack that has been returned to the pool and
reallocated to a new imp.**

## Actors

1. **Imp A** — running on stack S. It is about to exit (tail of `worker_loop` /
   `run()` calling `destroy_imp`).
2. **destroy_imp** — called by Imp A at the end of its lifetime. It:
   a. Switches away from stack S to the scheduler's main stack (`do_switch` to
      a new fcontext).
   b. After the switch, schedules the next imp.
   c. Returns stack S to the pool (`StackPool::release(S)` — acquires `mu_`,
      pushes S onto `free_list_`, releases `mu_`).
3. **Imp B** — a new imp being spawned. Its stack is allocated from the pool
   (`StackPool::allocate` — acquires `mu_`, pops from `free_list_`).
4. **StackPool** — the mutex-protected free list.

## Transition sequence (safe path)

1. Imp A decides to exit. It calls `do_switch` to jump to the processor's main
   stack. **At this point Imp A's stack frame is no longer being used.** The
   fcontext has been abandoned.
2. The processor's `worker_loop` resumes on the main stack (not on S).
3. `worker_loop` calls `destroy_imp(A)`.
4. `destroy_imp` acquires `mu_`, pushes S onto `free_list_`, releases `mu_`.
5. A new imp B is spawned. `StackPool::allocate` acquires `mu_`, pops S from
   `free_list_`, releases `mu_`. Stack S is now Imp B's stack.

This is safe because step 4 happens entirely after step 1 completes (the
switch is non-returning from Imp A's perspective).

## Hazard hypothesis

The race would occur if the stack is returned to the pool **before** the final
`do_switch` completes. Specifically:

- If `release(S)` is called before `do_switch` returns to the scheduler stack,
  a concurrent allocator could hand S to a new imp, and S would be in use by
  two execution contexts simultaneously.

**In the current implementation this cannot happen** because `release(S)` is
called from `destroy_imp`, which runs on the **scheduler's main stack** (not on
S) after `do_switch` has already transferred control away from S. The switch is
irreversible — once `do_switch` is called, the old fcontext on S is dead.

## Invariant to verify

`Stack S is in the free_list ⟹ no active fcontext is running on S`

Equivalently: `allocated(S) ⟹ S ∉ free_list`

## TLA+ scope

Model two imps (A, B) and one stack (S). States for S: `in_use_A`,
`in_transit` (A has switched off, destroy_imp not yet called release),
`in_pool`, `in_use_B`. Show that `in_pool` and `in_use_*` are mutually
exclusive (no double-allocation).

Bug variant: allow `release(S)` before `do_switch` completes — i.e., S enters
`in_pool` while still `in_use_A`. The allocator can then put S into `in_use_B`
concurrently, violating the invariant.
