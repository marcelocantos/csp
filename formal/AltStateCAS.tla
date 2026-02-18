---- MODULE AltStateCAS ----
(*******************************************************************************
 * Models the multi-waker alt claim protocol from csp/src/channel.cc.
 *
 * When a microthread (waiter) is registered on multiple channels via
 * alt/prialt, multiple peer threads (wakers) may simultaneously try to
 * claim it. The CAS on alt_state ensures exactly one waker succeeds.
 *
 * Participants:
 *   Waiter  — the microthread registered on multiple channels
 *   Waker_i — peer threads that find the waiter on their channel
 *
 * Code references (src/channel.cc, include/csp/internal/csp_internal.h):
 *   WaiterRegister  — channel.cc:301: alt_state.store(ALT_WAITING)
 *   WakerCAS        — channel.cc:131-132: alt_state.compare_exchange_strong(
 *                      ALT_WAITING, ALT_CLAIMED)
 *   WakerSetSignal  — channel.cc:133-134: signal_ = ~idx
 *   WakerSchedule   — channel.cc:135: schedule()
 *   LoserSkip       — channel.cc:132: CAS fails, continue loop
 *
 * AltState enum (csp_internal.h:74):
 *   ALT_IDLE=0, ALT_WAITING=1, ALT_CLAIMED=2
 ******************************************************************************)

EXTENDS Integers

CONSTANTS
    Wakers      \* set of waker IDs, e.g. {1, 2, 3}

VARIABLES
    alt_state,  \* "idle" | "waiting" | "claimed" — the waiter's alt_state
    signal,     \* 0 (none) or waker ID — which channel won
    on_queue,   \* Boolean: waiter has been scheduled (on a run queue)
    pc_waiter,  \* waiter's program counter
    pc_waker    \* [Wakers -> pc state]: per-waker program counter

vars == <<alt_state, signal, on_queue, pc_waiter, pc_waker>>

(*******************************************************************************
 * Initial state: waiter is idle, wakers are ready.
 ******************************************************************************)
Init ==
    /\ alt_state = "idle"
    /\ signal = "none"
    /\ on_queue = FALSE
    /\ pc_waiter = "will_register"
    /\ pc_waker = [w \in Wakers |-> "idle"]

(*******************************************************************************
 * WAITER ACTIONS
 *
 * The waiter registers on all channels, setting alt_state = WAITING.
 * Then it suspends (context-switches out). When woken, it proceeds to done.
 *
 * Code: alt_state.store(ALT_WAITING, release)  [channel.cc:301]
 *       do_switch(Status::detach)               [channel.cc:321]
 *       alt_state.store(ALT_IDLE, release)      [channel.cc:335]
 ******************************************************************************)

(* Waiter sets alt_state to WAITING and suspends.
 * This is one atomic step because the store and suspend happen before
 * any channel lock is released — wakers can only see the waiter on
 * the wait queue after unlock_all, at which point alt_state is already
 * WAITING. *)
WaiterRegister ==
    /\ pc_waiter = "will_register"
    /\ alt_state' = "waiting"
    /\ pc_waiter' = "suspended"
    /\ UNCHANGED <<signal, on_queue, pc_waker>>

(* Waiter has been woken (on_queue = TRUE) and proceeds to cleanup.
 * In the real code this is: wake from do_switch, lock_all, cleanup
 * registrations, set alt_state = IDLE.
 *
 * Code: alt_state.store(ALT_IDLE, release)  [channel.cc:335] *)
WaiterDone ==
    /\ pc_waiter = "suspended"
    /\ on_queue = TRUE
    /\ alt_state' = "idle"
    /\ pc_waiter' = "done"
    /\ UNCHANGED <<signal, on_queue, pc_waker>>

(*******************************************************************************
 * WAKER ACTIONS
 *
 * Each waker attempts a CAS on alt_state: WAITING -> CLAIMED.
 * Only one can succeed. The winner sets signal and calls schedule().
 * Losers skip (do nothing for this waiter).
 *
 * The CAS is a single atomic instruction (compare_exchange_strong),
 * so it's one TLA+ action.
 ******************************************************************************)

(* Waker becomes active — models a peer arriving on a channel where
 * the waiter is registered. Only enabled after waiter has registered.
 *
 * Code: release() iterating waiters, or prialt_begin_impl scanning peers *)
WakerStart(w) ==
    /\ pc_waker[w] = "idle"
    /\ alt_state = "waiting"
    /\ pc_waker' = [pc_waker EXCEPT ![w] = "try_cas"]
    /\ UNCHANGED <<alt_state, signal, on_queue, pc_waiter>>

(* Waker attempts CAS: WAITING -> CLAIMED.
 * Success: proceed to set signal.
 * Failure: alt_state is not WAITING (already claimed), skip.
 *
 * Code: alt_state.compare_exchange_strong(expected=ALT_WAITING, ALT_CLAIMED)
 *       [channel.cc:131-132] *)
WakerCAS(w) ==
    /\ pc_waker[w] = "try_cas"
    /\ IF alt_state = "waiting"
       THEN /\ alt_state' = "claimed"
            /\ pc_waker' = [pc_waker EXCEPT ![w] = "set_signal"]
            /\ UNCHANGED <<signal, on_queue, pc_waiter>>
       ELSE /\ pc_waker' = [pc_waker EXCEPT ![w] = "loser"]
            /\ UNCHANGED <<alt_state, signal, on_queue, pc_waiter>>

(* Winning waker sets signal to identify which channel won.
 *
 * Code: cw.thread->signal_ = ~idx  [channel.cc:133-134]
 * This is a plain store — safe because only the CAS winner reaches here. *)
WakerSetSignal(w) ==
    /\ pc_waker[w] = "set_signal"
    /\ signal' = w
    /\ pc_waker' = [pc_waker EXCEPT ![w] = "schedule"]
    /\ UNCHANGED <<alt_state, on_queue, pc_waiter>>

(* Winning waker calls schedule() to put waiter on a run queue.
 *
 * Code: cw.thread->schedule()  [channel.cc:135] *)
WakerSchedule(w) ==
    /\ pc_waker[w] = "schedule"
    /\ on_queue' = TRUE
    /\ pc_waker' = [pc_waker EXCEPT ![w] = "done_waker"]
    /\ UNCHANGED <<alt_state, signal, pc_waiter>>

(* Losing waker: CAS failed, move on. No further action for this waiter.
 *
 * Code: CAS returns false, loop continues to next waiter/channel *)
LoserDone(w) ==
    /\ pc_waker[w] = "loser"
    /\ pc_waker' = [pc_waker EXCEPT ![w] = "done_waker"]
    /\ UNCHANGED <<alt_state, signal, on_queue, pc_waiter>>

(*******************************************************************************
 * SPECIFICATION
 ******************************************************************************)

Next ==
    \/ WaiterRegister
    \/ WaiterDone
    \/ \E w \in Wakers :
        \/ WakerStart(w)
        \/ WakerCAS(w)
        \/ WakerSetSignal(w)
        \/ WakerSchedule(w)
        \/ LoserDone(w)

Spec == Init /\ [][Next]_vars

(*******************************************************************************
 * PROPERTIES
 ******************************************************************************)

(* Type invariant. *)
TypeOK ==
    /\ alt_state \in {"idle", "waiting", "claimed"}
    /\ signal \in ({"none"} \cup Wakers)
    /\ on_queue \in BOOLEAN
    /\ pc_waiter \in {"will_register", "suspended", "done"}
    /\ pc_waker \in [Wakers -> {"idle", "try_cas", "set_signal",
                                 "schedule", "loser", "done_waker"}]

(* Safety: at most one waker wins the CAS. We verify this by checking
 * that at most one waker is in the winner path {set_signal, schedule}.
 * These states are only reachable via the CAS success branch. *)
ExactlyOneClaim ==
    \A w1 \in Wakers : \A w2 \in Wakers :
        (w1 /= w2) =>
            ~(pc_waker[w1] \in {"set_signal", "schedule"}
              /\ pc_waker[w2] \in {"set_signal", "schedule"})

(* Safety: signal value corresponds to the winning waker. If signal is set
 * to some waker w, then w must be the one that won the CAS (past or present
 * in the winner path). *)
SignalConsistency ==
    \A w \in Wakers :
        (signal = w) =>
            pc_waker[w] \in {"schedule", "done_waker"}

(* Safety: once all wakers are done and at least one was active, the waiter
 * must be on a run queue. This ensures no lost wakeup. *)
WaiterWakes ==
    (\A w \in Wakers : pc_waker[w] = "done_waker")
    => (on_queue \/ alt_state = "idle")

====
