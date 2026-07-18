---- MODULE DrainSuspended ----
(*******************************************************************************
 * Models the suspend/wake protocol from csp/src/csp.cc (🎯T34 O2).
 *
 * An imp (MT) that blocks on a channel/timer suspends in two stages:
 * it announces the suspend window, detaches from its run queue, and
 * context-switches; the thread that resumes a different imp then runs
 * drain_suspended for it.  A waker calling mt->schedule() during the
 * window must neither lose the wake (imp sleeps forever) nor deliver
 * it early (imp queued and run before its context is saved — double
 * execution).
 *
 * Historically this was closed by two flags (suspending_/wake_pending_)
 * whose drain ran under the GLOBAL runtime mutex on every context
 * switch — the dominant scalability cost found in paper 33.  The
 * protocol is now a single per-imp atomic word:
 *
 *   suspend_state_ ∈ { IDLE, SUSP, WAKE }
 *
 *   MT begin:   suspend_state_.store(SUSP, release); unlock; run(detach)
 *   MT CheckWP: (in run(), before the context save, under run_mu)
 *               CAS(WAKE → IDLE): success ⇒ early wake — re-add self to
 *               the local queue and return without switching.
 *   Drain:      (after the context save, on the resuming thread)
 *               old = suspend_state_.exchange(IDLE, acq_rel)
 *               old = WAKE ⇒ push MT to a run queue (deferred wake).
 *   Waker:      loop { s = load(suspend_state_):
 *                 SUSP → CAS(SUSP → WAKE): success ⇒ done (defer)
 *                 WAKE → done (wake already pending)
 *                 IDLE → normal push path (imp fully suspended) }
 *
 * Every transition is a CAS/exchange on one word, so drain and waker
 * are linearizable without the global mutex; only the actual queue
 * push still takes it.  Wake exclusivity during the window comes from
 * the alt_state ALT_WAITING→ALT_CLAIMED handshake (AltStateCAS.tla).
 *
 * DrainSuspended_Bug.tla replaces Drain's atomic exchange with a
 * non-atomic load-then-store; TLC finds the lost wakeup.
 *
 * Code references (src/csp.cc, src/channel.cc, src/blocking_pool.cc):
 *   BeginSuspend — suspend_state_.store(SUSP); unlock; do_switch(detach)
 *   CheckWP      — run() detach path CAS(WAKE→IDLE)
 *   Drain        — drain_suspended(): exchange(IDLE)
 *   WakerCAS     — schedule(): CAS(SUSP→WAKE) / observe WAKE
 *   WakerPush    — schedule(): IDLE ⇒ push under global_mu
 *******************************************************************************)

EXTENDS Integers

VARIABLES
    state,          \* "idle" | "susp" | "wake" — the per-imp atomic word
    on_queue,       \* Boolean: MT is on some run queue
    waker_pushed,   \* Boolean: the waker (not MT/drain) queued the MT
    pc_mt,          \* MT / drain program counter
    pc_waker        \* Waker program counter

vars == <<state, on_queue, waker_pushed, pc_mt, pc_waker>>

Init ==
    /\ state = "idle"
    /\ on_queue = TRUE
    /\ waker_pushed = FALSE
    /\ pc_mt = "running"
    /\ pc_waker = "idle"

(*******************************************************************************
 * MT ACTIONS
 *******************************************************************************)

(* MT announces the suspend window and leaves the run queue.  One
 * action: the store happens before any channel unlock, so no waker can
 * observe intermediate state (it can only claim the MT after the
 * unlock).  TLA:DrainSuspended.BeginSuspend *)
BeginSuspend ==
    /\ pc_mt = "running"
    /\ state' = "susp"
    /\ on_queue' = FALSE
    /\ pc_mt' = "check_wp"
    /\ UNCHANGED <<waker_pushed, pc_waker>>

(* In run()'s detach path, before the context save: CAS(WAKE → IDLE).
 * Success ⇒ the wake arrived early; re-add to the local queue and keep
 * running (no context switch, so no drain will follow).
 * Failure (state is SUSP) ⇒ proceed to the context switch.
 * TLA:DrainSuspended.CheckWP *)
CheckWP ==
    /\ pc_mt = "check_wp"
    /\ IF state = "wake"
       THEN /\ state' = "idle"
            /\ on_queue' = TRUE
            /\ pc_mt' = "done"
       ELSE /\ pc_mt' = "switched_out"
            /\ UNCHANGED <<state, on_queue>>
    /\ UNCHANGED <<waker_pushed, pc_waker>>

(* After the context save, the resuming thread drains: one atomic
 * exchange(IDLE).  Seeing WAKE ⇒ the waker deferred; push now.
 * TLA:DrainSuspended.Drain *)
Drain ==
    /\ pc_mt = "switched_out"
    /\ IF state = "wake"
       THEN on_queue' = TRUE
       ELSE UNCHANGED on_queue
    /\ state' = "idle"
    /\ pc_mt' = "done"
    /\ UNCHANGED <<waker_pushed, pc_waker>>

(*******************************************************************************
 * WAKER ACTIONS
 *
 * Enabled only after BeginSuspend: a peer can claim the MT only after
 * it registered on channel wait queues, and the SUSP store is ordered
 * before the channel unlock that exposes the registration.
 *******************************************************************************)

StartWake ==
    /\ pc_waker = "idle"
    /\ pc_mt /= "running"
    /\ pc_waker' = "waking"
    /\ UNCHANGED <<state, on_queue, waker_pushed, pc_mt>>

(* One iteration of schedule()'s state loop, as three atomic cases on
 * the word.  TLA:DrainSuspended.WakerCAS TLA:DrainSuspended.WakerPush *)
WakerStep ==
    /\ pc_waker = "waking"
    /\ IF state = "susp"
       THEN \* CAS(SUSP → WAKE) — drain or CheckWP will queue the MT.
            /\ state' = "wake"
            /\ pc_waker' = "done_waker"
            /\ UNCHANGED <<on_queue, waker_pushed, pc_mt>>
       ELSE IF state = "wake"
       THEN \* Wake already pending — nothing to do.
            /\ pc_waker' = "done_waker"
            /\ UNCHANGED <<state, on_queue, waker_pushed, pc_mt>>
       ELSE \* IDLE: the MT is fully suspended (drain ran) — normal
            \* push path under global_mu (in_global_/next_ guards make
            \* the push idempotent; modeled by setting on_queue).
            /\ on_queue' = TRUE
            /\ waker_pushed' = TRUE
            /\ pc_waker' = "done_waker"
            /\ UNCHANGED <<state, pc_mt>>

Next ==
    \/ BeginSuspend
    \/ CheckWP
    \/ Drain
    \/ StartWake
    \/ WakerStep

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 *******************************************************************************)

TypeOK ==
    /\ state \in {"idle", "susp", "wake"}
    /\ on_queue \in BOOLEAN
    /\ waker_pushed \in BOOLEAN
    /\ pc_mt \in {"running", "check_wp", "switched_out", "done"}
    /\ pc_waker \in {"idle", "waking", "done_waker"}

(* Safety: when both sides have completed, the MT is on a queue —
 * no wake is ever lost. *)
NoLostWakeup ==
    (pc_mt = "done" /\ pc_waker = "done_waker") => on_queue

(* Safety: the waker only queues the MT itself once the MT is fully
 * suspended (drain complete).  A push before the context save would be
 * double execution; a push before drain would race the exchange. *)
NoPrematurePush ==
    waker_pushed => pc_mt = "done"

====
