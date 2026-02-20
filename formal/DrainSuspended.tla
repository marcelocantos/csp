---- MODULE DrainSuspended ----
(*******************************************************************************
 * Models the drain_suspended / schedule() protocol from csp/src/csp.cc.
 *
 * The protocol ensures that when an imp (MT) suspends and a waker
 * calls schedule(), the wakeup is never lost — regardless of how the
 * two threads interleave.
 *
 * Participants:
 *   MT    — the suspending imp (+ the drain context that runs
 *           drain_suspended on its behalf after context switch)
 *   Waker — another thread calling mt->schedule()
 *
 * Code references (src/csp.cc, src/channel.cc):
 *   BeginSuspend  — channel.cc: suspending_.store(true); unlock_all()
 *   CheckWP       — csp.cc run(): wake_pending_.exchange(false)
 *   AcquireDrain  — csp.cc drain_suspended(): global_mu.lock()
 *   Drain         — csp.cc drain_suspended(): clear suspending, drain wp
 *   StartWake     — channel.cc release(): peer decides to call schedule()
 *   AcquireWake   — csp.cc schedule(): global_mu.lock()
 *   DoSchedule    — csp.cc schedule(): check suspending, act accordingly
 *******************************************************************************)

EXTENDS Integers

VARIABLES
    suspending,     \* Boolean: MT has signaled it's in the suspend window
    wake_pending,   \* Boolean: deferred wakeup, set by schedule()
    on_queue,       \* Boolean: MT is on some run queue
    global_mu,      \* "none" | "drain" | "sched" — who holds the mutex
    pc_mt,          \* MT's program counter (where it is in the protocol)
    pc_waker        \* Waker's program counter

vars == <<suspending, wake_pending, on_queue, global_mu, pc_mt, pc_waker>>

(*******************************************************************************
 * Initial state: MT is running on a queue, nothing pending.
 *******************************************************************************)
Init ==
    /\ suspending = FALSE
    /\ wake_pending = FALSE
    /\ on_queue = TRUE
    /\ global_mu = "none"
    /\ pc_mt = "running"
    /\ pc_waker = "idle"

(*******************************************************************************
 * MT ACTIONS
 *
 * The MT goes through these phases:
 *   running → check_wp → switched_out → draining → done
 *                    ↘ clear_susp → done  (early wake path)
 *******************************************************************************)

(* MT begins suspending: sets the flag, leaves the run queue.
 * This is one atomic step because it happens before any unlock — no
 * other thread can observe the intermediate state.
 *
 * Code: suspending_.store(true, release); unlock_all();
 *       enter do_switch → run()
 *
 * TLA:DrainSuspended.BeginSuspend *)
BeginSuspend ==
    /\ pc_mt = "running"
    /\ suspending' = TRUE
    /\ on_queue' = FALSE
    /\ pc_mt' = "check_wp"
    /\ UNCHANGED <<wake_pending, global_mu, pc_waker>>

(* Inside run(), MT atomically exchanges wake_pending (under run_mu).
 * This is a single TLA+ action because atomic exchange is indivisible.
 *
 * If wake_pending was TRUE: the wakeup arrived early. MT re-adds itself
 * to the local queue and returns from do_switch (no context switch).
 * It still needs to clear suspending (next step).
 *
 * If wake_pending was FALSE: no wakeup yet. MT context-switches out
 * and awaits drain_suspended.
 *
 * Code: if (g_imp->wake_pending_.exchange(false, acq_rel)) {
 *           re-add to queue; return;
 *       } else { jump_fcontext(...); }
 *
 * TLA:DrainSuspended.CheckWP *)
CheckWP ==
    /\ pc_mt = "check_wp"
    /\ IF wake_pending
       THEN /\ wake_pending' = FALSE
            /\ on_queue' = TRUE
            /\ pc_mt' = "clear_susp"
       ELSE /\ pc_mt' = "switched_out"
            /\ UNCHANGED <<wake_pending, on_queue>>
    /\ UNCHANGED <<suspending, global_mu, pc_waker>>

(* After early wake: the caller (prialt_begin_impl / sleep_until)
 * clears suspending after do_switch returns. This is a separate
 * action because it happens AFTER run() returns — another thread
 * could observe suspending=TRUE in between.
 *
 * Code: do_switch(Status::detach);
 *       g_imp->suspending_.store(false, release);  // after return
 *
 * TLA:DrainSuspended.ClearSusp *)
ClearSusp ==
    /\ pc_mt = "clear_susp"
    /\ suspending' = FALSE
    /\ pc_mt' = "done"
    /\ UNCHANGED <<wake_pending, on_queue, global_mu, pc_waker>>

(* After context switch: another thread runs drain_suspended for this
 * MT. First, acquire global_mu.
 *
 * Code: std::lock_guard<std::mutex> lk(rt.global_mu);
 *
 * TLA:DrainSuspended.AcquireDrain *)
AcquireDrain ==
    /\ pc_mt = "switched_out"
    /\ global_mu = "none"
    /\ global_mu' = "drain"
    /\ pc_mt' = "draining"
    /\ UNCHANGED <<suspending, wake_pending, on_queue, pc_waker>>

(* drain_suspended (under global_mu): clear suspending, atomically
 * exchange wake_pending, push to queue if wake was pending.
 * Release lock.
 *
 * All three operations (clear suspending, check+clear wp, push) are
 * one TLA+ action because they're all under the same lock — no other
 * thread can see intermediate states.
 *
 * Code: suspended->suspending_.store(false, release);
 *       if (suspended->wake_pending_.exchange(false, acq_rel)) {
 *           if (!suspended->in_global_)
 *               rt.push_to_global(suspended);
 *       }
 *
 * TLA:DrainSuspended.Drain *)
Drain ==
    /\ pc_mt = "draining"
    /\ suspending' = FALSE
    /\ IF wake_pending
       THEN /\ wake_pending' = FALSE
            /\ on_queue' = TRUE
       ELSE UNCHANGED <<wake_pending, on_queue>>
    /\ global_mu' = "none"
    /\ pc_mt' = "done"
    /\ UNCHANGED pc_waker

(*******************************************************************************
 * WAKER ACTIONS
 *
 * The waker calls mt->schedule(). In the real code, this is triggered
 * by a channel peer (e.g., a writer finding a blocked reader).
 * The waker can only act after the MT has started suspending — before
 * that, the MT isn't registered on any channel wait queue.
 *******************************************************************************)

(* Waker decides to call schedule(). Only enabled after MT starts
 * suspending (pc_mt != "running"), modeling the fact that the MT must
 * first register on channel wait queues before a peer can find it.
 *
 * TLA:DrainSuspended.StartWake *)
StartWake ==
    /\ pc_waker = "idle"
    /\ pc_mt /= "running"
    /\ pc_waker' = "want_lock"
    /\ UNCHANGED <<suspending, wake_pending, on_queue, global_mu, pc_mt>>

(* Waker acquires global_mu.
 *
 * TLA:DrainSuspended.AcquireWake *)
AcquireWake ==
    /\ pc_waker = "want_lock"
    /\ global_mu = "none"
    /\ global_mu' = "sched"
    /\ pc_waker' = "scheduling"
    /\ UNCHANGED <<suspending, wake_pending, on_queue, pc_mt>>

(* schedule() under global_mu: check suspending flag.
 * If suspending: can't push to queue (MT is mid-switch). Defer via wp.
 * If not suspending: push to queue directly.
 * Release lock.
 *
 * Single TLA+ action because everything is under the same lock.
 *
 * Code: if (suspending_.load(acquire)) {
 *           wake_pending_.store(true, release);
 *           return;
 *       }
 *       rt.push_to_global(this);
 *
 * TLA:DrainSuspended.DoSchedule *)
DoSchedule ==
    /\ pc_waker = "scheduling"
    /\ IF suspending
       THEN /\ wake_pending' = TRUE
            /\ UNCHANGED on_queue
       ELSE /\ on_queue' = TRUE
            /\ UNCHANGED wake_pending
    /\ global_mu' = "none"
    /\ pc_waker' = "done_waker"
    /\ UNCHANGED <<suspending, pc_mt>>

(*******************************************************************************
 * SPECIFICATION
 *
 * The system starts in Init and takes Next steps forever.
 * [][Next]_vars means: each step either satisfies Next or stutters
 * (leaves all variables unchanged). Stuttering models the environment
 * doing nothing — it lets TLC verify safety without requiring progress.
 *******************************************************************************)

Next ==
    \/ BeginSuspend
    \/ CheckWP
    \/ ClearSusp
    \/ AcquireDrain
    \/ Drain
    \/ StartWake
    \/ AcquireWake
    \/ DoSchedule

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 *
 * These are checked by TLC in every reachable state.
 *******************************************************************************)

(* Type invariant: variables stay in their expected domains.
 * Catches modeling errors — if a variable takes an unexpected value,
 * the spec has a bug. *)
TypeOK ==
    /\ suspending \in BOOLEAN
    /\ wake_pending \in BOOLEAN
    /\ on_queue \in BOOLEAN
    /\ global_mu \in {"none", "drain", "sched"}
    /\ pc_mt \in {"running", "check_wp", "clear_susp",
                   "switched_out", "draining", "done"}
    /\ pc_waker \in {"idle", "want_lock", "scheduling", "done_waker"}

(* Safety: when both threads have completed, the MT must be on a queue.
 * If this is violated, a wakeup was lost — the MT will never run again. *)
NoLostWakeup ==
    (pc_mt = "done" /\ pc_waker = "done_waker") => on_queue

====
